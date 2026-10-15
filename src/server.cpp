#include "duckdb.hpp"
#include "duckonwire/json.hpp"
#include "duckonwire/pgwire.hpp"
#include "duckonwire/access.hpp"
#include "duckonwire/native_access.hpp"
#include "duckonwire/read_copy.hpp"
#include "duckonwire/peer_guard.hpp"
#include "initialize.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
volatile std::sig_atomic_t stop_requested = 0;

void RequestStop(int) {
	stop_requested = 1;
}

using duckonwire::JsonString;

std::string RandomToken() {
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) throw std::runtime_error("Cannot open the native random source");
	unsigned char bytes[32];
	size_t offset = 0;
	while (offset < sizeof(bytes)) {
		auto count = read(fd, bytes + offset, sizeof(bytes) - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			close(fd);
			throw std::runtime_error("Cannot read the native random source");
		}
		offset += static_cast<size_t>(count);
	}
	close(fd);
	const char hex[] = "0123456789abcdef";
	std::string token;
	for (auto byte : bytes) {
		token += hex[byte >> 4];
		token += hex[byte & 15];
	}
	return token;
}

bool Within(const fs::path &path, const fs::path &root) {
	auto part = path.begin();
	for (auto base = root.begin(); base != root.end(); ++base, ++part) {
		if (part == path.end() || *part != *base) return false;
	}
	return part != path.end();
}

fs::path OwnedPath(const fs::path &input, const fs::path &root) {
	auto path = fs::weakly_canonical(input.is_absolute() ? input : root / input);
	if (!Within(path, root)) throw std::runtime_error("Paths must remain inside --root");
	return path;
}

fs::path ConfigurationPath(const fs::path &input, const fs::path &root) {
	auto requested = (input.is_absolute() ? input : root / input).lexically_normal();
	OwnedPath(requested, root);
	if (fs::is_symlink(fs::symlink_status(requested)))
		throw std::runtime_error("Configuration and TLS files must not be symlinks");
	return requested;
}

struct DatabaseOption {
	std::string name;
	fs::path path, spill, secrets;
	fs::path pointer, versions;
};

std::string DatabaseKey(std::string_view value) {
	std::string result(value);
	for (char &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	return result;
}

bool DatabaseName(std::string_view value) {
	if (value.empty() || value.size() > 63) return false;
	auto alnum = [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_';
	};
	if (!alnum(static_cast<unsigned char>(value.front()))) return false;
	for (unsigned char c : value) if (!alnum(c) && c != '-' && c != '.') return false;
	const auto key = DatabaseKey(value);
	return key != "system" && key != "temp";
}

struct Options {
	fs::path root, state;
	std::vector<DatabaseOption> databases;
	fs::path access_file, tls_cert, tls_key;
	std::string pg_host = "127.0.0.1";
	int port = 9495;
	int pg_port = 9496;
	int max_queries = 8;
	int query_timeout_ms = 30000;
	int threads = 0;
	duckonwire::PeerGuardOptions peer_guard;
	bool init_demo = false;
	std::int64_t demo_prices = 0;
};

void ValidateStoragePaths(const Options &options) {
	// A database also owns its WAL and spill namespace. Never let one engine's
	// storage or cleanup touch another database, the descriptor, or a TLS/rules file.
	const bool protected_mode = !options.access_file.empty();
	std::vector<fs::path> resources {OwnedPath(options.state, options.root),
	                                OwnedPath(options.state.string() + ".update", options.root)};
	for (const auto &database : options.databases) {
		if (database.pointer.empty()) {
			resources.push_back(OwnedPath(database.path, options.root));
			for (const auto &suffix : {".wal", ".wal.checkpoint", ".wal.recovery", ".tmp"})
				resources.push_back(OwnedPath(fs::path(database.path.string() + suffix), options.root));
		} else {
			resources.push_back(OwnedPath(database.pointer, options.root));
			resources.push_back(OwnedPath(database.pointer.string() + ".active", options.root));
			resources.push_back(OwnedPath(database.pointer.string() + ".active.update", options.root));
			resources.push_back(OwnedPath(database.versions, options.root));
		}
		resources.push_back(OwnedPath(database.spill, options.root));
		if (protected_mode) resources.push_back(OwnedPath(database.secrets, options.root));
	}
	if (protected_mode) {
		resources.push_back(OwnedPath(options.access_file, options.root));
		resources.push_back(OwnedPath(options.tls_cert, options.root));
		resources.push_back(OwnedPath(options.tls_key, options.root));
	}
	for (std::size_t i = 0; i < resources.size(); ++i) {
		for (std::size_t j = 0; j < i; ++j) {
			std::error_code error;
			const bool same_file = fs::equivalent(resources[i], resources[j], error);
			if (resources[i] == resources[j] || Within(resources[i], resources[j]) ||
			    Within(resources[j], resources[i]) || (!error && same_file))
				throw std::runtime_error("Database, WAL/recovery, temporary, spill, secrets, descriptor and configuration paths must not overlap");
		}
	}
}

Options Parse(int argc, char **argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		std::string name(argv[i]);
		if (name == "--init-demo") { options.init_demo = true; continue; }
		if (name == "--help") {
			std::cout << "duckonwire-server [--root DIRECTORY] [--database PATH | NAME=PATH] [--port 9495] [--pg-port 9496] [--state PATH] [--init-demo] [--demo-prices ROWS] [--threads 1..64]\n"
			          << "Quack uses --port; PostgreSQL drivers use --pg-port (0 selects a free port).\n"
			          << "Repeat --database NAME=PATH for up to 32 isolated databases. Only the first may omit NAME.\n"
			          << "--read-copy NAME=POINTER publishes immutable read-only copies; atomically replace private POINTER with a version basename in POINTER.versions (private directory).\n"
			          << "Quack uses consecutive loopback ports, one per database; choose a PostgreSQL port outside that range.\n"
			          << "DUCKONWIRE_TOKEN sets local endpoint passwords; otherwise one is generated.\n"
			          << "Public: --pg-host ADDRESS --access-file PATH --tls-cert PATH --tls-key PATH\n"
			          << "PostgreSQL limits: --max-queries 8 --query-timeout-ms 30000\n"
			          << "Engine: --threads 1..64 per database; default uses DuckDB's detected CPU capacity, capped at 2 workers per database.\n"
			          << "Demo: --init-demo imports the catalog; --demo-prices 1000000 adds reproducible 30-day synthetic history instead of CSV prices (46462..10000000 rows).\n"
			          << "Access rules assign users read/write permission within named database sections; TLS and SCRAM required.\n";
			std::cout << "Protected peer limits: --auth-max-failures 5 --auth-window-seconds 60 --ban-seconds 300\n"
			          << "--max-connections-per-minute 120 --max-requests-per-minute 6000 (per socket IP, shared by all databases and protocols).\n";
			std::exit(0);
		}
		if (i + 1 == argc) throw std::runtime_error("Every option requires a value");
		std::string value(argv[++i]);
		if (name == "--root") options.root = value;
		else if (name == "--database" || name == "--read-copy") {
			if (options.databases.size() >= 32) throw std::runtime_error("At most 32 databases may be served");
			const auto separator = value.find('=');
			DatabaseOption database;
			if (separator == std::string::npos) {
				if (name == "--read-copy") throw std::runtime_error("Read copies require NAME=POINTER");
				if (!options.databases.empty()) throw std::runtime_error("Additional databases require NAME=PATH");
				database.path = value;
			} else {
				database.name = value.substr(0, separator);
				database.path = value.substr(separator + 1);
				if (!DatabaseName(database.name)) throw std::runtime_error("Invalid database name");
			}
			if (database.path.empty() || value.find('\0') != std::string::npos)
				throw std::runtime_error("A database requires a nonempty file path");
			if (name == "--read-copy") database.pointer = database.path;
			options.databases.push_back(std::move(database));
		}
		else if (name == "--state") options.state = value;
		else if (name == "--access-file") options.access_file = value;
		else if (name == "--tls-cert") options.tls_cert = value;
		else if (name == "--tls-key") options.tls_key = value;
		else if (name == "--pg-host") options.pg_host = value;
		else if (name == "--auth-max-failures" || name == "--auth-window-seconds" || name == "--ban-seconds" ||
		         name == "--max-connections-per-minute" || name == "--max-requests-per-minute") {
			size_t consumed = 0;
			const auto number = std::stoull(value, &consumed);
			if (consumed != value.size() || value.empty() || value.front() == '-' || number < 1 || number > 1000000)
				throw std::runtime_error("Peer protection limits must be between 1 and 1000000");
			const auto limit = static_cast<std::uint32_t>(number);
			if (name == "--auth-max-failures") options.peer_guard.auth_failures = limit;
			else if (name == "--auth-window-seconds") options.peer_guard.auth_window_seconds = limit;
			else if (name == "--ban-seconds") options.peer_guard.ban_seconds = limit;
			else if (name == "--max-connections-per-minute") options.peer_guard.connections_per_minute = limit;
			else options.peer_guard.requests_per_minute = limit;
		}
		else if (name == "--threads") {
			size_t consumed = 0;
			options.threads = std::stoi(value, &consumed);
			if (consumed != value.size() || options.threads < 1 || options.threads > 64)
				throw std::runtime_error("Threads must be between 1 and 64");
		}
		else if (name == "--demo-prices") {
			size_t consumed = 0;
			options.demo_prices = std::stoll(value, &consumed);
			if (consumed != value.size() || options.demo_prices < 46462 || options.demo_prices > 10000000)
				throw std::runtime_error("Demo price rows must be between 46462 and 10000000");
		}
		else if (name == "--max-queries" || name == "--query-timeout-ms") {
			size_t consumed = 0;
			const int number = std::stoi(value, &consumed);
			const int maximum = name == "--max-queries" ? 256 : 3600000;
			if (consumed != value.size() || number < 1 || number > maximum)
				throw std::runtime_error("Invalid PostgreSQL capacity or timeout limit");
			if (name == "--max-queries") options.max_queries = number;
			else options.query_timeout_ms = number;
		}
		else if (name == "--port") {
			size_t consumed = 0;
			options.port = std::stoi(value, &consumed);
			if (consumed != value.size() || options.port < 1 || options.port > 65535) {
				throw std::runtime_error("Port must be between 1 and 65535");
			}
		} else if (name == "--pg-port") {
			size_t consumed = 0;
			options.pg_port = std::stoi(value, &consumed);
			if (consumed != value.size() || options.pg_port < 0 || options.pg_port > 65535) {
				throw std::runtime_error("PostgreSQL port must be between 0 and 65535");
			}
		} else throw std::runtime_error("Unknown option: " + name);
	}
	if (options.demo_prices != 0 && !options.init_demo)
		throw std::runtime_error("--demo-prices requires --init-demo");
	if (options.root.empty()) options.root = fs::current_path();
	options.root = fs::canonical(options.root);
	if (!fs::is_directory(options.root)) throw std::runtime_error("--root must be a directory");
	if (options.databases.empty()) options.databases.push_back({"", "data/demo.duckdb", {}, {}});
	if (options.databases.size() - 1 > static_cast<std::size_t>(65535 - options.port))
		throw std::runtime_error("The database Quack port range exceeds 65535");
	if (options.pg_port != 0 && options.pg_port >= options.port &&
	    options.pg_port < options.port + static_cast<int>(options.databases.size()))
		throw std::runtime_error("The PostgreSQL port overlaps a database Quack port; use --pg-port 0 or a different port");
	std::unordered_set<std::string> names;
	for (auto &database : options.databases) {
		if (!database.name.empty() && !names.insert(DatabaseKey(database.name)).second)
			throw std::runtime_error("Database names must be unique, ignoring case");
		if (!database.pointer.empty()) {
			if (options.init_demo) throw std::runtime_error("Read copies cannot be combined with --init-demo");
			database.pointer = (database.pointer.is_absolute() ? database.pointer : options.root / database.pointer).lexically_normal();
			database.path = duckonwire::ReadCopyController::InitialPath(options.root, database.pointer);
			database.versions = duckonwire::ReadCopyController::VersionDirectory(options.root, database.pointer);
			database.spill = OwnedPath(options.root / ".runtime" / ("read-copy-" + database.name + ".spill"), options.root);
			database.secrets = OwnedPath(options.root / ".runtime" / ("read-copy-" + database.name + ".secrets"), options.root);
			continue;
		}
		database.path = OwnedPath(database.path, options.root);
		database.spill = OwnedPath(options.databases.size() == 1 ? database.path.parent_path() / "native-spill" :
		                          fs::path(database.path.string() + ".spill"), options.root);
		database.secrets = OwnedPath(options.databases.size() == 1 ? options.root / ".runtime/engine-secrets" :
		                            fs::path(database.path.string() + ".secrets"), options.root);
	}
	options.state = OwnedPath(options.state.empty() ? fs::path(".runtime/native.json") : options.state, options.root);
	const bool protected_mode = !options.access_file.empty();
	if (!protected_mode && ((options.pg_host != "127.0.0.1" && options.pg_host != "localhost") || !options.tls_cert.empty() || !options.tls_key.empty()))
		throw std::runtime_error("Public listeners and TLS require --access-file with user rules");
	if (protected_mode) {
		if (options.tls_cert.empty() || options.tls_key.empty())
			throw std::runtime_error("Access rules require both --tls-cert and --tls-key");
		options.access_file = ConfigurationPath(options.access_file, options.root);
		options.tls_cert = ConfigurationPath(options.tls_cert, options.root);
		options.tls_key = ConfigurationPath(options.tls_key, options.root);
		struct stat private_key_status {};
		if (stat(options.tls_key.c_str(), &private_key_status) != 0 || !S_ISREG(private_key_status.st_mode) ||
		    ((private_key_status.st_mode & 07777) != 0600 && (private_key_status.st_mode & 07777) != 0400))
			throw std::runtime_error("TLS private key must be a regular file with mode 0600 or 0400");
	}
	ValidateStoragePaths(options);
	return options;
}

class Descriptor {
public:
	explicit Descriptor(const fs::path &path) : path(path) {
		fs::create_directories(path.parent_path());
		fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) {
			if (errno == EEXIST) throw std::runtime_error("Descriptor already exists; inspect its owner before starting");
			throw std::runtime_error("Cannot create the private descriptor");
		}
		if (fstat(fd, &identity) != 0 || fchmod(fd, 0600) != 0) {
			close(fd);
			fd = -1;
			unlink(path.c_str());
			throw std::runtime_error("Cannot secure the descriptor");
		}
	}
	~Descriptor() {
		if (fd >= 0) close(fd);
		struct stat current {};
		if (lstat(path.c_str(), &current) == 0 && current.st_dev == identity.st_dev && current.st_ino == identity.st_ino) {
			unlink(path.c_str());
		}
	}
	void Write(const std::string &payload) {
		const bool updating = fd < 0;
		fs::path temporary;
		if (updating) {
			struct stat current {};
			if (lstat(path.c_str(), &current) != 0 || current.st_dev != identity.st_dev || current.st_ino != identity.st_ino)
				throw std::runtime_error("Private descriptor owner changed");
			std::string temporary_name = path.string() + ".update.XXXXXX";
			fd = mkstemp(temporary_name.data());
			if (fd < 0) throw std::runtime_error("Cannot create private descriptor update");
			temporary = temporary_name;
		}
		try {
		if (updating && (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0600) != 0))
			throw std::runtime_error("Cannot secure private descriptor update");
		size_t offset = 0;
		while (offset < payload.size()) {
			auto count = write(fd, payload.data() + offset, payload.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) throw std::runtime_error("Cannot write the private descriptor");
			offset += static_cast<size_t>(count);
		}
		if (fsync(fd) != 0) throw std::runtime_error("Cannot persist the private descriptor");
		struct stat updated {};
		if (fstat(fd, &updated) != 0) throw std::runtime_error("Cannot inspect private descriptor update");
		close(fd);
		fd = -1;
		if (updating) {
			struct stat current {};
			if (lstat(path.c_str(), &current) != 0 || current.st_dev != identity.st_dev || current.st_ino != identity.st_ino)
				throw std::runtime_error("Private descriptor owner changed");
			if (rename(temporary.c_str(), path.c_str()) != 0)
				throw std::runtime_error("Cannot publish private descriptor update");
		}
		identity = updated;
		} catch (...) {
			if (fd >= 0) close(fd);
			fd = -1;
			if (updating) unlink(temporary.c_str());
			throw;
		}
	}
private:
	fs::path path;
	int fd = -1;
	struct stat identity {};
};

template <class... Args>
void Query(duckdb::Connection &connection, const std::string &sql, Args... args) {
	auto result = connection.Query(sql, args...);
	if (result->HasError()) result->ThrowError();
}

void InstallSignals() {
	struct sigaction action {};
	action.sa_handler = RequestStop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, nullptr) != 0 || sigaction(SIGINT, &action, nullptr) != 0) {
		throw std::runtime_error("Cannot install native termination handlers");
	}
	action.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &action, nullptr) != 0) throw std::runtime_error("Cannot protect native socket writes");
}

void Serve(const Options &options, const std::string &token) {
	const bool protected_mode = !options.access_file.empty();
	duckonwire::AccessConfig rules;
	std::shared_ptr<duckonwire::PeerGuard> peer_guard;
	if (protected_mode) {
		if (options.init_demo) throw std::runtime_error("Initialize the demo locally before enabling access rules");
		rules = duckonwire::ReadAccessConfig(options.access_file.string());
		peer_guard = std::make_shared<duckonwire::PeerGuard>(options.peer_guard);
	}
	auto configured = options.databases;
	if (configured.front().name.empty()) configured.front().name = protected_mode ? rules.database : "duckonwire";
	std::unordered_set<std::string> names;
	for (const auto &database : configured) {
		if (!DatabaseName(database.name) || !names.insert(DatabaseKey(database.name)).second)
			throw std::runtime_error("Database names must be valid and unique, ignoring case");
	}
	if (protected_mode) {
		std::unordered_set<std::string> permission_names;
		for (const auto &name : rules.databases) {
			if (!names.count(DatabaseKey(name))) throw std::runtime_error("Access rules reference an unconfigured database");
			permission_names.insert(DatabaseKey(name));
		}
		for (const auto &database : configured) {
			if (!permission_names.count(DatabaseKey(database.name)))
				throw std::runtime_error("Each protected database requires an access-rules section");
		}
	}
	Descriptor descriptor(options.state);
	struct DatabaseRuntime {
		DatabaseOption settings;
		std::string endpoint;
		std::unique_ptr<duckdb::DuckDB> database;
		std::unique_ptr<duckonwire::ReadCopyController> read_copy;
		std::unique_ptr<duckdb::Connection> owner;
		bool active = false;
	};
	std::vector<DatabaseRuntime> databases;
	databases.reserve(configured.size());
	duckdb::idx_t shared_memory_budget = 0;
	for (std::size_t i = 0; i < configured.size(); ++i) {
		const auto &settings = configured[i];
		const bool read_copy = !settings.pointer.empty();
		fs::create_directories(settings.path.parent_path());
		// Creation can reveal aliases that were absent during Parse on a
		// case-insensitive filesystem. Recheck before opening the next engine.
		ValidateStoragePaths(options);
		duckdb::DBConfig config;
		config.SetOptionByName(duckdb::Identifier("temp_directory"), duckdb::Value(settings.spill.string()));
		if (options.threads != 0)
			config.SetOptionByName(duckdb::Identifier("threads"), duckdb::Value::BIGINT(options.threads));
		if (protected_mode || read_copy) {
			config.SetOptionByName(duckdb::Identifier("enable_external_access"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("allow_unsigned_extensions"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("allow_persistent_secrets"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("secret_directory"), duckdb::Value(settings.secrets.string()));
			config.SetOptionByName(duckdb::Identifier("max_temp_directory_size"), duckdb::Value("1GiB"));
		}
		DatabaseRuntime runtime;
		runtime.settings = settings;
		runtime.endpoint = "quack:127.0.0.1:" + std::to_string(options.port + static_cast<int>(i));
		runtime.database = std::make_unique<duckdb::DuckDB>(read_copy ? nullptr : settings.path.c_str(), &config);
		if (read_copy) {
			// The native filesystem exists only after construction. These fixed
			// storage exceptions are installed once, before any client can run.
			auto &storage_config = duckdb::DBConfig::GetConfig(*runtime.database->instance);
			storage_config.AddAllowedDirectory(settings.versions.string());
			storage_config.AddAllowedDirectory(settings.spill.string());
			runtime.read_copy = std::make_unique<duckonwire::ReadCopyController>(*runtime.database, options.root, settings.pointer,
			    [&options](const fs::path &) { ValidateStoragePaths(options); });
		}
		runtime.owner = std::make_unique<duckdb::Connection>(*runtime.database);
		auto &database = *runtime.database;
		auto &owner = *runtime.owner;
		if (protected_mode) {
			duckonwire::ConfigureNativeAccess(database, settings.name, rules, peer_guard, read_copy,
			    read_copy ? runtime.read_copy->Available() : nullptr);
			// Retain quota detection and reserve process headroom. Independent
			// engines share this one buffer budget rather than multiplying it.
			constexpr duckdb::idx_t memory_cap = 1024ULL * 1024ULL * 1024ULL;
			auto &actual = duckdb::DBConfig::GetConfig(*database.instance);
			if (i == 0)
				shared_memory_budget = std::min(memory_cap, actual.options.maximum_memory - actual.options.maximum_memory / 4);
			const auto budget = shared_memory_budget / configured.size();
			if (!budget) throw std::runtime_error("Insufficient buffer memory for the configured databases");
			auto memory_option = duckdb::DBConfig::GetOptionByName(duckdb::Identifier("memory_limit"));
			if (!memory_option) throw std::runtime_error("Native memory setting is unavailable");
			actual.SetOption(database.instance.get(), *memory_option, duckdb::Value(std::to_string(budget) + "B"));
		}
		if (!protected_mode && read_copy)
			duckonwire::ConfigureReadCopyNativeAccess(database, token, runtime.read_copy->Available());
		if (options.threads == 0) {
			// Let DuckDB detect CPU quotas before applying our two-worker default cap.
			auto detected = owner.Query("SELECT current_setting('threads')");
			if (detected->HasError()) detected->ThrowError();
			if (detected->GetValue<int64_t>(0, 0) > 2) Query(owner, "SET threads = 2");
		}
		if (options.init_demo && i == 0) {
			duckonwire::demo::Initialize(owner, options.root, options.demo_prices);
		}
		if (!protected_mode && !read_copy) {
			// duckdb_extensions() enumerates the user's extension directory. As
			// in protected mode, copies never access that external directory.
			auto builtins = owner.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
			if (builtins->HasError()) builtins->ThrowError();
			if (builtins->GetValue<int64_t>(0, 0) != 2) throw std::runtime_error("This server requires the integrated native DuckDB build");
		}
		databases.push_back(std::move(runtime));
	}
	std::vector<duckonwire::PgDatabase> routes;
	for (const auto &runtime : databases) routes.push_back({runtime.settings.name, runtime.database.get(),
	    static_cast<bool>(runtime.read_copy), runtime.read_copy ? runtime.read_copy->Available() : nullptr,
	    runtime.read_copy ? runtime.read_copy->StorageDirectory().string() : std::string()});
	std::unique_ptr<duckonwire::PgWireServer> pg_owner;
	if (protected_mode) {
		duckonwire::PgWireOptions settings;
		settings.host = options.pg_host;
		settings.port = static_cast<std::uint16_t>(options.pg_port);
		settings.database_name = databases.front().settings.name;
		settings.databases = routes;
		settings.max_active_queries = static_cast<std::size_t>(options.max_queries);
		settings.query_timeout_ms = static_cast<std::uint32_t>(options.query_timeout_ms);
		settings.peer_guard = peer_guard;
		settings.tls_cert = options.tls_cert.string();
		settings.tls_key = options.tls_key.string();
		for (const auto &rule : rules.users) {
			auto configured_name = std::find_if(configured.begin(), configured.end(), [&](const DatabaseOption &database) {
				return DatabaseKey(database.name) == DatabaseKey(rule.database);
			});
			if (configured_name == configured.end()) throw std::runtime_error("Access rules reference an unconfigured database");
			settings.users.push_back({rule.name, rule.verifier, rule.read_only, configured_name->name});
		}
		pg_owner = std::make_unique<duckonwire::PgWireServer>(*databases.front().database, std::move(settings));
	} else {
		pg_owner = std::make_unique<duckonwire::PgWireServer>(*databases.front().database, options.pg_host,
		    static_cast<std::uint16_t>(options.pg_port), token, static_cast<std::size_t>(options.max_queries),
		    static_cast<std::uint32_t>(options.query_timeout_ms), std::move(routes));
	}
	auto &postgres = *pg_owner;
	auto stop_services = [&]() noexcept {
		postgres.Stop();
		std::exception_ptr error;
		for (auto runtime = databases.rbegin(); runtime != databases.rend(); ++runtime) {
			if (!runtime->active) continue;
			try {
				Query(*runtime->owner, "CALL quack_stop(?)", duckdb::Value(runtime->endpoint));
				runtime->active = false;
			} catch (...) { if (!error) error = std::current_exception(); }
		}
		return error;
	};
	try {
		for (auto &runtime : databases) {
			Query(*runtime.owner, "CALL quack_serve(?, token=?)", duckdb::Value(runtime.endpoint), duckdb::Value(token));
			runtime.active = true;
			if (protected_mode || runtime.read_copy) {
				Query(*runtime.owner, "SET TimeZone = 'UTC'");
				Query(*runtime.owner, "SET lock_configuration = true");
			}
		}
		postgres.Start();
		const auto &primary = databases.front();
		auto shared_state = [&]() {
			std::string database_list = "[";
			for (const auto &runtime : databases) {
				if (database_list.size() > 1) database_list += ',';
				database_list += "{\"name\":" + JsonString(runtime.settings.name) +
				    ",\"path\":" + JsonString(runtime.read_copy ? runtime.read_copy->Path().string() : runtime.settings.path.string()) +
				    ",\"endpoint\":" + JsonString(runtime.endpoint);
				if (runtime.read_copy) database_list += ",\"read_copy_pointer\":" + JsonString(runtime.settings.pointer.string()) +
				    ",\"read_copy_directory\":" + JsonString(runtime.read_copy->StorageDirectory().string()) +
				    ",\"generation\":" + std::to_string(runtime.read_copy->Generation()) +
				    ",\"available\":" + (runtime.read_copy->Available()->load() ? "true" : "false");
				database_list += '}';
			}
			database_list += ']';
			return "\"pid\":" + std::to_string(getpid()) +
		    ",\"pg_endpoint\":" + JsonString(postgres.Endpoint()) +
		    ",\"pg_port\":" + std::to_string(postgres.Port()) +
		    ",\"database\":" + JsonString(primary.read_copy ? primary.read_copy->Path().string() : primary.settings.path.string()) +
		    ",\"databases\":" + database_list + ",\"builtin_extensions\":[\"httpfs\",\"quack\"]";
		};
		auto write_descriptor = [&]() {
			descriptor.Write("{\"endpoint\":" + JsonString(primary.endpoint) + ",\"token\":" + JsonString(token) + "," + shared_state() +
			                 ",\"duckdb\":" + JsonString(duckdb::DuckDB::LibraryVersion()) + "}\n");
		};
		write_descriptor();
		const auto shared = shared_state();
		std::cout << "{\"ready\":true,\"uri\":" << JsonString(primary.endpoint) << "," << shared
		          << ",\"duckdb_version\":" << JsonString(duckdb::DuckDB::LibraryVersion())
		          << ",\"source_id\":" << JsonString(duckdb::DuckDB::SourceID()) << "}" << std::endl;
		bool metadata_dirty = false, metadata_error_logged = false;
		auto next_metadata_attempt = std::chrono::steady_clock::now();
		while (!stop_requested) {
			struct timespec interval {0, 100000000};
			nanosleep(&interval, nullptr);
			for (auto &runtime : databases) {
				if (!runtime.read_copy) continue;
				const auto result = runtime.read_copy->Poll();
				using Result = duckonwire::ReadCopyController::Result;
				if (result == Result::Unchanged) continue;
				const char *status = result == Result::Published ? "published" : result == Result::Restored ? "restored" :
				                     result == Result::Unavailable ? "unavailable" : "rejected";
				std::cerr << "{\"event\":\"read-copy-refresh\",\"database\":" << JsonString(runtime.settings.name)
				          << ",\"status\":" << JsonString(status) << ",\"generation\":" << runtime.read_copy->Generation()
				          << (result == Result::VersionLimit ? ",\"reason\":\"version-limit\"" : "") << "}" << std::endl;
				if (result != Result::Rejected && result != Result::VersionLimit) metadata_dirty = true;
			}
			if (metadata_dirty && std::chrono::steady_clock::now() >= next_metadata_attempt) {
				try {
					write_descriptor(); metadata_dirty = false; metadata_error_logged = false;
				} catch (...) {
					if (!metadata_error_logged)
						std::cerr << "{\"event\":\"read-copy-metadata\",\"status\":\"failed\"}" << std::endl;
					metadata_error_logged = true;
					next_metadata_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
				}
			}
		}
		if (auto error = stop_services()) std::rethrow_exception(error);
	} catch (...) {
		stop_services();
		throw;
	}
	// PostgreSQL and Quack sessions, Connection and DuckDB close before descriptor removal.
}
} // namespace

int main(int argc, char **argv) {
	std::string token;
	try {
		auto options = Parse(argc, argv);
		InstallSignals();
		const char *configured_token = std::getenv("DUCKONWIRE_TOKEN");
		token = configured_token ? configured_token : RandomToken();
		Serve(options, token);
		return 0;
	} catch (const std::exception &error) {
		std::string message(error.what());
		if (!token.empty()) {
			size_t position = 0;
			while ((position = message.find(token, position)) != std::string::npos) {
				message.replace(position, token.size(), "[redacted]");
				position += std::strlen("[redacted]");
			}
		}
		std::cerr << "duckonwire-server: " << message << '\n';
		return 1;
	}
}
