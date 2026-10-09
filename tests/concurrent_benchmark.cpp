#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "support/native_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <sys/utsname.h>
#include <thread>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif

using namespace duckdb;
using test_support::Check;
using test_support::Require;
using test_support::FetchRows;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {
struct Options {
	uint64_t rows = 2000000, batch = 25000;
	unsigned writers = 4, readers = 4;
	int timeout = 300;
	std::string mode = "both";
	std::string environment = "unspecified";
	fs::path server, output, temporary_parent = fs::temp_directory_path();
};

std::string Json(const std::string &value) {
	std::string result = "\"";
	const char *hex = "0123456789abcdef";
	for (unsigned char byte : value) {
		if (byte == '\"' || byte == '\\') { result += '\\'; result += byte; }
		else if (byte < 32) {
			result += "\\u00";
			result += hex[byte >> 4]; result += hex[byte & 15];
		} else result += byte;
	}
	return result + "\"";
}

uint64_t Number(const std::string &text) {
	Require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
	        "Numeric options require positive whole numbers");
	return std::stoull(text);
}

Options Parse(int argc, char **argv) {
	Options options;
	options.server = fs::absolute(argv[0]).parent_path() / "duckonwire-server";
	for (int i = 1; i < argc; ++i) {
		const std::string argument = argv[i];
		if (argument == "--help") {
			std::cout << "duckonwire-benchmark [--mode both|embedded|quack] [--rows 2000000]\n"
			             "  [--writers 4] [--readers 4] [--batch 25000] [--timeout 300]\n"
			             "  [--server build/bin/duckonwire-server] [--temporary-parent DIR] [--output FILE]\n"
			             "  [--environment 'AWS Sao Paulo']\n"
			             "Each mode uses a fresh owned database. JSON goes to stdout and optional FILE.\n";
			std::exit(0);
		}
		Require(i + 1 < argc, "Every option requires a value");
		const std::string value = argv[++i];
		if (argument == "--mode") options.mode = value;
		else if (argument == "--rows") options.rows = Number(value);
		else if (argument == "--batch") options.batch = Number(value);
		else if (argument == "--writers") { const auto n = Number(value); Require(n <= 64, "Too many writers"); options.writers = n; }
		else if (argument == "--readers") { const auto n = Number(value); Require(n <= 64, "Too many readers"); options.readers = n; }
		else if (argument == "--timeout") { const auto n = Number(value); Require(n <= 3600, "Timeout exceeds one hour"); options.timeout = n; }
		else if (argument == "--server") options.server = value;
		else if (argument == "--output") options.output = value;
		else if (argument == "--temporary-parent") options.temporary_parent = value;
		else if (argument == "--environment") options.environment = value;
		else throw std::runtime_error("Unknown option: " + argument);
	}
	Require(options.mode == "both" || options.mode == "embedded" || options.mode == "quack", "Unknown benchmark mode");
	Require(options.rows >= options.writers && options.rows <= 100000000 && options.batch > 0 &&
	        options.writers > 0 && options.readers > 0 && options.timeout > 0, "Invalid benchmark limits");
	Require(!options.environment.empty() && options.environment.size() <= 80, "Environment label must contain 1-80 bytes");
	for (const unsigned char byte : options.environment) {
		Require(byte >= 128 || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
		        (byte >= '0' && byte <= '9') || byte == ' ' || byte == '-' || byte == '_' || byte == '+' ||
		        byte == '(' || byte == ')' || byte == ',', "Environment label must be a human name, without addresses or paths");
	}
	return options;
}

std::string UTCNow() {
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	struct tm utc {};
	Require(gmtime_r(&now, &utc) != nullptr, "Cannot read UTC workload time");
	std::ostringstream output;
	output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return output.str();
}

double Seconds(Clock::time_point beginning) {
	return std::chrono::duration<double>(Clock::now() - beginning).count();
}

struct Samples {
	std::vector<double> ms;
	uint64_t while_writers_active = 0;
	void Merge(const Samples &other) {
		ms.insert(ms.end(), other.ms.begin(), other.ms.end());
		while_writers_active += other.while_writers_active;
	}
	std::string Serialize() const {
		double sum = 0;
		for (const auto value : ms) sum += value;
		auto ordered = ms;
		std::sort(ordered.begin(), ordered.end());
		auto percentile = [&](double fraction) {
			return ordered.empty() ? 0.0 : ordered[static_cast<size_t>(std::ceil(fraction * ordered.size())) - 1];
		};
		std::ostringstream output;
		output << std::setprecision(10) << "{\"samples\":" << ms.size()
		       << ",\"completed_while_writers_active\":" << while_writers_active
		       << ",\"mean_ms\":" << (ms.empty() ? 0 : sum / ms.size())
		       << ",\"p50_ms\":" << percentile(0.5) << ",\"p95_ms\":" << percentile(0.95)
		       << ",\"max_ms\":" << (ordered.empty() ? 0 : ordered.back()) << "}";
		return output.str();
	}
};

struct Worker {
	std::unique_ptr<Connection> connection;
	duckdb::unique_ptr<PreparedStatement> prepared;
	std::map<std::string, Samples> samples;
	uint64_t batches = 0, inserted_rows = 0, initial_count = 0, final_count = 0;
	double finished_seconds = 0;
};

struct Gate {
	std::mutex mutex;
	std::condition_variable condition;
	size_t ready = 0;
	bool started = false;
	Clock::time_point beginning;
	void Wait() {
		std::unique_lock<std::mutex> lock(mutex);
		++ready;
		condition.notify_all();
		condition.wait(lock, [&] { return started; });
	}
	void Start(size_t participants) {
		std::unique_lock<std::mutex> lock(mutex);
		condition.wait(lock, [&] { return ready == participants; });
		beginning = Clock::now();
		started = true;
		condition.notify_all();
	}
};

uint64_t CentSum(uint64_t rows) {
	const uint64_t cycle = 100000;
	uint64_t sum = rows / cycle * (cycle * (cycle - 1) / 2 + cycle * 100);
	for (uint64_t i = 0; i < rows % cycle; ++i) sum += (i * 37) % cycle + 100;
	return sum;
}

struct Integrity {
	uint64_t count, id_sum, cent_sum, product_sum;
	std::string Serialize() const {
		return "{\"count\":" + std::to_string(count) + ",\"id_checksum\":" + std::to_string(id_sum) +
		       ",\"price_cents_checksum\":" + std::to_string(cent_sum) +
		       ",\"product_id_checksum\":" + std::to_string(product_sum) + ",\"passed\":true}";
	}
};

Integrity Validate(Connection &connection, const Options &options, uint64_t products) {
	auto result = connection.Query("SELECT count(*),CAST(sum(id) AS BIGINT),CAST(sum(CAST(price*100 AS BIGINT)) AS BIGINT),"
	                               "CAST(sum(product_id) AS BIGINT),min(id),max(id),count(DISTINCT id),"
	                               "count(*) FILTER (WHERE product_id != id % ? OR CAST(price*100 AS BIGINT) != (id*37)%100000+100 "
	                               "OR day != DATE '2026-10-09' - CAST(id%30 AS INTEGER)) FROM prices", Value::BIGINT(products));
	Check(result);
	const auto values = FetchRows(*result);
	Require(values.size() == 1, "Integrity query must return one row");
	Integrity integrity {static_cast<uint64_t>(values[0][0].GetValue<int64_t>()),
	                     static_cast<uint64_t>(values[0][1].GetValue<int64_t>()),
	                     static_cast<uint64_t>(values[0][2].GetValue<int64_t>()),
	                     static_cast<uint64_t>(values[0][3].GetValue<int64_t>())};
	const auto expected_products = options.rows / products * (products * (products - 1) / 2) +
	                               (options.rows % products) * (options.rows % products - 1) / 2;
	Require(integrity.count == options.rows && integrity.id_sum == options.rows * (options.rows - 1) / 2 &&
	        integrity.cent_sum == CentSum(options.rows) && integrity.product_sum == expected_products &&
	        values[0][4].GetValue<int64_t>() == 0 && values[0][5].GetValue<int64_t>() == static_cast<int64_t>(options.rows - 1) &&
	        values[0][6].GetValue<int64_t>() == static_cast<int64_t>(options.rows) && values[0][7].GetValue<int64_t>() == 0,
	        "Exact durable row count/checksums/domain integrity failed");
	auto partitions = connection.Query("SELECT writer,count(*),min(id),max(id) FROM prices GROUP BY writer ORDER BY writer");
	Check(partitions);
	Require(partitions->RowCount() == options.writers, "Writer partition is missing");
	for (unsigned i = 0; i < options.writers; ++i) {
		const uint64_t start = options.rows * i / options.writers, end = options.rows * (i + 1) / options.writers;
		Require(partitions->GetValue<int64_t>(0, i) == i && partitions->GetValue<int64_t>(1, i) == static_cast<int64_t>(end - start) &&
		        partitions->GetValue<int64_t>(2, i) == static_cast<int64_t>(start) &&
		        partitions->GetValue<int64_t>(3, i) == static_cast<int64_t>(end - 1), "Writer partition boundaries differ");
	}
	auto product_count = connection.Query("SELECT count(*) FROM products");
	Check(product_count);
	Require(product_count->GetValue<int64_t>(0, 0) == static_cast<int64_t>(products), "Catalog rows changed");
	return integrity;
}

std::unique_ptr<Connection> Open(DuckDB &database, test_support::NativeServer *server) {
	auto result = std::make_unique<Connection>(database);
	if (server) test_support::Connect(*result, server->endpoint(), server->token());
	return result;
}

#ifdef __linux__
bool Numeric(const std::string &text, uint64_t &value) {
	if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
	try { value = std::stoull(text); return true; } catch (...) { return false; }
}

fs::path CgroupPath(const fs::path &mount, const std::string &controller) {
	std::ifstream membership("/proc/self/cgroup");
	std::string line;
	while (std::getline(membership, line)) {
		const auto first = line.find(':'), second = first == std::string::npos ? first : line.find(':', first + 1);
		if (second == std::string::npos) continue;
		const std::string controllers = "," + line.substr(first + 1, second - first - 1) + ",";
		if (controller.empty() ? controllers != ",," : controllers.find("," + controller + ",") == std::string::npos) continue;
		const fs::path relative = fs::path(line.substr(second + 1)).relative_path();
		bool safe = true;
		for (const auto &part : relative) if (part == "..") safe = false;
		std::error_code error;
		if (safe && fs::is_directory(mount / relative, error)) return mount / relative;
	}
	return mount; // A container namespace often exposes its current cgroup as the mount root.
}

std::string ContainerLimits(unsigned visible_cpus, uint64_t visible_memory) {
	std::optional<double> quota;
	std::optional<uint64_t> memory;
	bool cpu_read = false, memory_read = false;
	std::string cpu_version, memory_version;
	auto cpu = [&](const fs::path &mount, bool v2) {
		for (auto path = CgroupPath(mount, v2 ? "" : "cpu"); ; path = path.parent_path()) {
			std::string limit, period;
			std::ifstream limits(path / (v2 ? "cpu.max" : "cpu.cfs_quota_us"));
			if (limits >> limit) {
				if (v2) limits >> period;
				else { std::ifstream interval(path / "cpu.cfs_period_us"); interval >> period; }
				uint64_t numerator = 0, denominator = 0;
				if ((limit == "max" || limit == "-1" || Numeric(limit, numerator)) && Numeric(period, denominator) && denominator > 0) {
					cpu_read = true;
					cpu_version = v2 ? "v2" : "v1";
					if (numerator > 0) {
						const double current = static_cast<double>(numerator) / denominator;
						quota = quota ? std::min(*quota, current) : current;
					}
				}
			}
			if (path == mount || path.empty() || path == path.parent_path()) break;
		}
	};
	auto ram = [&](const fs::path &mount, bool v2) {
		for (auto path = CgroupPath(mount, v2 ? "" : "memory"); ; path = path.parent_path()) {
			std::string limit;
			std::ifstream limits(path / (v2 ? "memory.max" : "memory.limit_in_bytes"));
			uint64_t bytes = 0;
			if ((limits >> limit) && (limit == "max" || Numeric(limit, bytes))) {
				memory_read = true;
				memory_version = v2 ? "v2" : "v1";
				// v1 uses a near-INT64_MAX page-rounded sentinel for unlimited memory.
				if (limit != "max" && (v2 || bytes < (uint64_t(1) << 60))) memory = memory ? std::min(*memory, bytes) : bytes;
			}
			if (path == mount || path.empty() || path == path.parent_path()) break;
		}
	};
	cpu("/sys/fs/cgroup", true);
	ram("/sys/fs/cgroup", true);
	if (!cpu_read) {
		for (const auto &mount : {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct", "/sys/fs/cgroup/cpuacct,cpu"}) {
			cpu(mount, false);
			if (cpu_read) break;
		}
	}
	if (!memory_read) ram("/sys/fs/cgroup/memory", false);
	std::optional<unsigned> affinity;
	cpu_set_t mask;
	CPU_ZERO(&mask);
	if (sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_COUNT(&mask) > 0) affinity = CPU_COUNT(&mask);
	std::optional<double> effective_cpu;
	if (visible_cpus) effective_cpu = visible_cpus;
	if (affinity) effective_cpu = effective_cpu ? std::min(*effective_cpu, static_cast<double>(*affinity)) : *affinity;
	if (quota) effective_cpu = effective_cpu ? std::min(*effective_cpu, *quota) : *quota;
	std::optional<uint64_t> effective_memory;
	if (visible_memory) effective_memory = visible_memory;
	if (memory) effective_memory = effective_memory ? std::min(*effective_memory, *memory) : *memory;
	std::string version = cpu_version.empty() ? memory_version : cpu_version;
	if (!memory_version.empty() && !cpu_version.empty() && memory_version != cpu_version) version = "hybrid";
	if (version.empty()) version = "unavailable";
	std::ostringstream output;
	output << std::setprecision(12) << "{\"cgroup_version\":" << Json(version)
	       << ",\"cpu_controller_read\":" << (cpu_read ? "true" : "false")
	       << ",\"memory_controller_read\":" << (memory_read ? "true" : "false")
	       << ",\"cpu_quota_cores\":";
	if (quota) output << *quota; else output << "null";
	output << ",\"memory_limit_bytes\":";
	if (memory) output << *memory; else output << "null";
	output << ",\"cpu_affinity_count\":";
	if (affinity) output << *affinity; else output << "null";
	output << ",\"effective_cpu_cores\":";
	if (effective_cpu) output << *effective_cpu; else output << "null";
	output << ",\"effective_memory_bytes\":";
	if (effective_memory) output << *effective_memory; else output << "null";
	return output.str() + "}";
}
#endif

std::string Hardware() {
	struct utsname info {};
	uname(&info);
	std::string cpu = "unknown";
	uint64_t memory = 0;
#ifdef __APPLE__
	char bytes[256] {};
	size_t length = sizeof(bytes);
	if (sysctlbyname("machdep.cpu.brand_string", bytes, &length, nullptr, 0) == 0) cpu = bytes;
	length = sizeof(memory);
	sysctlbyname("hw.memsize", &memory, &length, nullptr, 0);
#else
	std::ifstream stream("/proc/cpuinfo");
	std::string line;
	while (std::getline(stream, line)) {
		if (line.rfind("model name", 0) == 0) { cpu = line.substr(line.find(':') + 2); break; }
	}
	const auto pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page_size > 0) memory = static_cast<uint64_t>(pages) * page_size;
#endif
	std::string result = "{\"os\":" + Json(info.sysname) + ",\"os_release\":" + Json(info.release) +
	       ",\"architecture\":" + Json(info.machine) + ",\"cpu\":" + Json(cpu) +
	       ",\"logical_cpu_count\":" + std::to_string(std::thread::hardware_concurrency()) +
	       ",\"physical_memory_bytes\":" + std::to_string(memory);
#ifdef __linux__
	result += ",\"host_visible\":{\"logical_cpu_count\":" + std::to_string(std::thread::hardware_concurrency()) +
	          ",\"physical_memory_bytes\":" + std::to_string(memory) + "},\"container_limits\":" +
	          ContainerLimits(std::thread::hardware_concurrency(), memory);
#endif
	return result + "}";
}

std::string RunMode(const Options &options, bool remote) {
	std::string report;
	fs::path owned_path;
	{
		test_support::TempRoot fixture(options.temporary_parent);
		owned_path = fixture.path();
		const auto file = fixture.path() / "benchmark.duckdb";
		auto setup_start = Clock::now();
		std::unique_ptr<test_support::NativeServer> server;
		if (remote) server = std::make_unique<test_support::NativeServer>(fixture.path(), file, options.server);
		DBConfig config;
		config.SetOptionByName("threads", Value::BIGINT(2));
		auto database = std::make_unique<DuckDB>(remote ? nullptr : file.string().c_str(), &config);
		auto owner = Open(*database, server.get());
		auto builtins = owner->Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') "
		                            "AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
		Check(builtins);
		Require(builtins->GetValue<int64_t>(0, 0) == 2, "Native built-in HTTPFS and Quack are required");
		builtins.reset();
		auto threads = owner->Query("SELECT current_setting('threads'),current_setting('memory_limit')");
		Check(threads);
		const auto engine_threads = threads->GetValue<int64_t>(0, 0);
		const auto engine_memory_limit = threads->GetValue(1, 0).GetValue<std::string>();
		threads.reset();
		const uint64_t products = std::min<uint64_t>(20000, options.rows);
		Check(owner->Query("CREATE TABLE products(id INTEGER PRIMARY KEY,name VARCHAR,category INTEGER,country_code VARCHAR)"));
		Check(owner->Query("CREATE TABLE prices(id BIGINT PRIMARY KEY,product_id INTEGER,writer INTEGER,price DECIMAL(18,2),day DATE)"));
		Check(owner->Query("INSERT INTO products SELECT i,'Product '||i,i%24,CASE WHEN i%2=0 THEN 'AR' ELSE 'ES' END FROM range(?) t(i)",
		                   Value::BIGINT(products)));
		std::vector<Worker> writers(options.writers), readers(options.readers);
		for (auto &writer : writers) {
			writer.connection = Open(*database, server.get());
			writer.prepared = writer.connection->Prepare("INSERT INTO prices SELECT i,CAST(i%? AS INTEGER),?,"
			    "CAST(((i*37)%100000+100)*0.01 AS DECIMAL(18,2)),DATE '2026-10-09'-CAST(i%30 AS INTEGER) FROM range(?,?) t(i)");
			Check(writer.prepared);
		}
		for (auto &reader : readers) {
			reader.connection = Open(*database, server.get());
			reader.prepared = reader.connection->Prepare("SELECT p.name,CAST(r.price*100 AS BIGINT) FROM prices r "
			    "JOIN products p ON p.id=r.product_id WHERE r.id=?");
			Check(reader.prepared);
		}
		const double setup_seconds = Seconds(setup_start);
		Gate gate;
		std::atomic<unsigned> active_writers {options.writers}, finished_workers {0};
		std::atomic<uint64_t> committed_batches {0};
		std::atomic<bool> aborted {false};
		std::mutex errors_mutex;
		std::vector<std::string> errors;
		auto failure = [&](const std::exception &error) {
			aborted = true;
			std::lock_guard<std::mutex> lock(errors_mutex);
			errors.push_back(error.what());
		};
		std::vector<std::thread> workers;
		for (unsigned index = 0; index < options.writers; ++index) {
			workers.emplace_back([&, index] {
				auto &writer = writers[index];
				gate.Wait();
				try {
					const uint64_t begin = options.rows * index / options.writers;
					const uint64_t end = options.rows * (index + 1) / options.writers;
					for (uint64_t start = begin; start < end && !aborted; start += options.batch) {
						const uint64_t next = std::min(end, start + options.batch);
						const auto transaction_start = Clock::now();
						writer.connection->BeginTransaction();
						const auto insert_start = Clock::now();
						duckdb::vector<Value> parameters {Value::BIGINT(products), Value::INTEGER(index),
						                                  Value::BIGINT(start), Value::BIGINT(next)};
						auto inserted = writer.prepared->Execute(parameters, false);
						Check(inserted);
						const auto changed = FetchRows(*inserted);
						Require(changed.size() == 1 && changed[0][0].GetValue<int64_t>() == static_cast<int64_t>(next - start), "Bound INSERT affected-row count differs");
						writer.samples["insert"].ms.push_back(Seconds(insert_start) * 1000);
						writer.connection->Commit();
						writer.samples["transaction"].ms.push_back(Seconds(transaction_start) * 1000);
						writer.inserted_rows += next - start;
						++writer.batches;
						++committed_batches;
					}
				} catch (const std::exception &error) {
					failure(error);
					try { writer.connection->Rollback(); } catch (...) {}
				}
				writer.finished_seconds = Seconds(gate.beginning);
				--active_writers;
				++finished_workers;
			});
		}
		for (unsigned index = 0; index < options.readers; ++index) {
			workers.emplace_back([&, index] {
				auto &reader = readers[index];
				gate.Wait();
				try {
					uint64_t iteration = 0, previous_count = 0;
					do {
						const auto query_start = Clock::now();
						const auto kind = (iteration + index) % 4;
						std::string name;
						if (kind == 0) {
							name = "point_join";
							const auto id = (iteration * 15485863 + index * 49999) % options.rows;
							duckdb::vector<Value> parameters {Value::BIGINT(id)};
							auto row = reader.prepared->Execute(parameters, false);
							Check(row);
							const auto rows = FetchRows(*row);
							Require(rows.size() <= 1, "Point query returned duplicate IDs");
							if (!rows.empty()) Require(rows[0][0].GetValue<std::string>() == "Product " + std::to_string(id % products) &&
							    rows[0][1].GetValue<int64_t>() == static_cast<int64_t>((id * 37) % 100000 + 100), "Point query read corrupt data");
						} else if (kind == 1) {
							name = "count_checksum";
							auto count = reader.connection->Query("SELECT count(*),CAST(coalesce(sum(id),0) AS BIGINT) FROM prices");
							Check(count);
							const auto current = count->GetValue<int64_t>(0, 0);
							Require(current >= static_cast<int64_t>(previous_count) && current <= static_cast<int64_t>(options.rows), "Reader count decreased or exceeded inserted rows");
							if (reader.samples[name].ms.empty()) reader.initial_count = current;
							reader.final_count = previous_count = current;
						} else if (kind == 2) {
							name = "join_group_by";
							auto groups = reader.connection->Query("SELECT p.country_code,p.category,count(*),sum(r.price) FROM prices r "
							    "JOIN products p ON p.id=r.product_id GROUP BY p.country_code,p.category ORDER BY p.country_code,p.category");
							Check(groups);
							Require(groups->RowCount() <= 48, "Grouped query returned invalid catalog groups");
						} else {
							name = "latest_price_window";
							auto window = reader.connection->Query("SELECT country_code,category,count(*),sum(price) FROM ("
							    "SELECT p.country_code,p.category,r.price,row_number() OVER(PARTITION BY p.id ORDER BY r.day DESC,r.id DESC) AS position "
							    "FROM products p JOIN prices r ON r.product_id=p.id WHERE p.id<256) latest "
							    "WHERE position=1 GROUP BY country_code,category ORDER BY country_code,category");
							Check(window);
							Require(window->RowCount() <= 48, "Window query returned invalid catalog groups");
						}
						auto &sample = reader.samples[name];
						sample.ms.push_back(Seconds(query_start) * 1000);
						if (active_writers > 0 && committed_batches > 0) ++sample.while_writers_active;
						++iteration;
					} while (!aborted && (active_writers > 0 || iteration < 4));
				} catch (const std::exception &error) { failure(error); }
				reader.finished_seconds = Seconds(gate.beginning);
				++finished_workers;
			});
		}
		gate.Start(options.writers + options.readers);
		const auto deadline = gate.beginning + std::chrono::seconds(options.timeout);
		while (finished_workers < options.writers + options.readers && Clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (finished_workers < options.writers + options.readers) {
			aborted = true;
			for (auto &writer : writers) writer.connection->Interrupt();
			for (auto &reader : readers) reader.connection->Interrupt();
			std::lock_guard<std::mutex> lock(errors_mutex);
			errors.push_back("Benchmark exceeded its native query deadline");
		}
		for (auto &worker : workers) worker.join();
		const double workload_seconds = Seconds(gate.beginning);
		Require(errors.empty(), "Benchmark worker failed" + (errors.empty() ? std::string() : ": " + errors.front()));
		std::map<std::string, Samples> reader_samples;
		Samples inserts, transactions, reads;
		double write_seconds = 0;
		uint64_t row_count = 0, batch_count = 0, overlap = 0;
		std::ostringstream client_report;
		for (unsigned i = 0; i < writers.size(); ++i) {
			const auto &writer = writers[i];
			row_count += writer.inserted_rows; batch_count += writer.batches;
			write_seconds = std::max(write_seconds, writer.finished_seconds);
			inserts.Merge(writer.samples.at("insert")); transactions.Merge(writer.samples.at("transaction"));
		}
		Require(row_count == options.rows, "Some writer rows were not committed");
		for (unsigned i = 0; i < readers.size(); ++i) {
			const auto &reader = readers[i];
			uint64_t own_overlap = 0, own_samples = 0;
			for (const auto &sample : reader.samples) {
				reader_samples[sample.first].Merge(sample.second);
				reads.Merge(sample.second);
				own_overlap += sample.second.while_writers_active; own_samples += sample.second.ms.size();
			}
			// Tiny smoke runs may finish before every individual reader completes its first query.
			if (options.rows >= 2000000) Require(own_overlap > 0, "Reader did not finish a query while writers were active");
			overlap += own_overlap;
			if (i) client_report << ',';
			client_report << "{\"reader\":" << i << ",\"samples\":" << own_samples << ",\"completed_while_writers_active\":" << own_overlap
			              << ",\"first_observed_count\":" << reader.initial_count << ",\"last_observed_count\":" << reader.final_count << "}";
		}
		Require(overlap > 0, "No reader completed a query while writers committed batches");
		const auto before = Validate(*owner, options, products);
		writers.clear(); readers.clear(); owner.reset(); database.reset();
		if (server) {
			server->stop();
			Require(server->clean_shutdown(),
			        "Native benchmark server did not shut down cleanly");
			server.reset();
		}
		Integrity after {};
		{
			if (remote) server = std::make_unique<test_support::NativeServer>(fixture.path(), file, options.server);
			DuckDB reopened(remote ? nullptr : file.string().c_str(), &config);
			auto reader = Open(reopened, server.get());
			after = Validate(*reader, options, products);
		}
		if (server) {
			server->stop();
			Require(server->clean_shutdown(),
			        "Reopened native benchmark server did not shut down cleanly");
			server.reset();
		}
		std::ostringstream output;
		output << std::setprecision(10) << "{\"mode\":" << Json(remote ? "quack_loopback" : "embedded")
		       << ",\"passed\":true,\"rows\":" << options.rows << ",\"catalog_products\":" << products
		       << ",\"writers\":" << options.writers << ",\"readers\":" << options.readers << ",\"engine_threads\":" << engine_threads
		       << ",\"engine_memory_limit\":" << Json(engine_memory_limit)
		       << ",\"batch_rows\":" << options.batch << ",\"committed_batches\":" << batch_count
		       << ",\"setup_seconds\":" << setup_seconds << ",\"write_seconds\":" << write_seconds
		       << ",\"workload_seconds\":" << workload_seconds << ",\"insert_rows_per_second\":" << options.rows / write_seconds
		       << ",\"insert_latency\":" << inserts.Serialize() << ",\"transaction_latency\":" << transactions.Serialize()
		       << ",\"read_latency\":" << reads.Serialize()
		       << ",\"query_latency\":{";
		bool first = true;
		for (const auto &sample : reader_samples) {
			if (!first) output << ',';
			first = false;
			output << Json(sample.first) << ':' << sample.second.Serialize();
		}
		output << "},\"reader_progress\":[" << client_report.str() << "],\"before_close\":" << before.Serialize()
		       << ",\"after_reopen\":" << after.Serialize() << ",\"durable_reopen_verified\":true,\"fixture_removed\":true}";
		report = output.str();
	}
	Require(!fs::exists(owned_path), "Owned benchmark fixture was not removed");
	return report;
}
} // namespace

int main(int argc, char **argv) {
	try {
		const auto options = Parse(argc, argv);
		const auto started_at_utc = UTCNow();
		std::vector<std::string> results;
		if (options.mode == "both" || options.mode == "embedded") {
			std::cerr << "Measuring embedded: " << options.rows << " rows, " << options.writers << " writers and " << options.readers << " readers\n";
			results.push_back(RunMode(options, false));
		}
		if (options.mode == "both" || options.mode == "quack") {
			std::cerr << "Measuring Quack loopback: " << options.rows << " rows, " << options.writers << " writers and " << options.readers << " readers\n";
			results.push_back(RunMode(options, true));
		}
		std::ostringstream json;
		json << "{\"passed\":true,\"native_used\":true,\"python_used\":false,\"duckdb_version\":"
		     << Json(DuckDB::LibraryVersion()) << ",\"source_id\":" << Json(DuckDB::SourceID())
		     << ",\"environment\":" << Json(options.environment) << ",\"workload_started_at_utc\":" << Json(started_at_utc)
		     << ",\"workload_finished_at_utc\":" << Json(UTCNow())
		     << ",\"hardware\":" << Hardware() << ",\"method\":{\"writes\":\"Typed prepared INSERT SELECT range, one explicit transaction per batch\","
		        "\"reads\":\"Point join, count/checksum, whole-table join/GROUP BY, latest-price window for 256 catalog products\","
		        "\"network\":\"Loopback only; remote-host latency is not measured\","
		        "\"comparison\":\"Sequential runs on separate fresh databases; OS caches are not flushed; setup excluded from insert throughput\","
		        "\"percentiles\":\"Nearest rank over completed query/transaction latency samples\"},\"results\":[";
		for (size_t i = 0; i < results.size(); ++i) { if (i) json << ','; json << results[i]; }
		json << "]}\n";
		if (!options.output.empty()) {
			if (!options.output.parent_path().empty()) fs::create_directories(options.output.parent_path());
			const fs::path temporary = options.output.string() + ".new";
			std::ofstream stream(temporary);
			stream << json.str();
			stream.close();
			Require(bool(stream), "Cannot persist benchmark output");
			fs::rename(temporary, options.output);
		}
		std::cout << json.str();
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "duckonwire-benchmark: " << error.what() << '\n';
		return 1;
	}
}
