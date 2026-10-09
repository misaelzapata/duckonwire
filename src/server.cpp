#include "duckdb.hpp"
#include "duckonwire/json.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
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

std::string SqlString(const std::string &value) {
	std::string result = "'";
	for (char byte : value) {
		result += byte;
		if (byte == '\'') result += '\'';
	}
	return result + "'";
}

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

struct Options {
	fs::path root, database, state;
	int port = 9495;
	bool init_demo = false;
};

Options Parse(int argc, char **argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		std::string name(argv[i]);
		if (name == "--init-demo") { options.init_demo = true; continue; }
		if (name == "--help") {
			std::cout << "duckonwire-server [--root DIRECTORY] [--database PATH] [--port 9495] [--state PATH] [--init-demo]\n"
			          << "DUCKONWIRE_TOKEN sets the server token; otherwise one is generated.\n";
			std::exit(0);
		}
		if (i + 1 == argc) throw std::runtime_error("Every option requires a value");
		std::string value(argv[++i]);
		if (name == "--root") options.root = value;
		else if (name == "--database") options.database = value;
		else if (name == "--state") options.state = value;
		else if (name == "--port") {
			size_t consumed = 0;
			options.port = std::stoi(value, &consumed);
			if (consumed != value.size() || options.port < 1 || options.port > 65535) {
				throw std::runtime_error("Port must be between 1 and 65535");
			}
		} else throw std::runtime_error("Unknown option: " + name);
	}
	if (options.root.empty()) options.root = fs::current_path();
	options.root = fs::canonical(options.root);
	if (!fs::is_directory(options.root)) throw std::runtime_error("--root must be a directory");
	options.database = OwnedPath(options.database.empty() ? fs::path("data/demo.duckdb") : options.database,
	                             options.root);
	options.state = OwnedPath(options.state.empty() ? fs::path(".runtime/native.json") : options.state, options.root);
	if (options.database == options.state) throw std::runtime_error("Database and descriptor paths must differ");
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
		size_t offset = 0;
		while (offset < payload.size()) {
			auto count = write(fd, payload.data() + offset, payload.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) throw std::runtime_error("Cannot write the private descriptor");
			offset += static_cast<size_t>(count);
		}
		if (fsync(fd) != 0) throw std::runtime_error("Cannot persist the private descriptor");
		close(fd);
		fd = -1;
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

void InitDemo(duckdb::Connection &owner, const fs::path &root) {
	auto tables = owner.Query("SELECT count(*) FROM duckdb_tables() WHERE NOT internal AND NOT temporary");
	if (tables->HasError()) tables->ThrowError();
	if (tables->GetValue<int64_t>(0, 0) != 0) {
		throw std::runtime_error("--init-demo requires an empty database; existing tables are preserved");
	}
	const auto products = OwnedPath("examples/demo/products.csv", root);
	const auto prices = OwnedPath("examples/demo/prices.csv", root);
	if (!fs::is_regular_file(products) || !fs::is_regular_file(prices)) {
		throw std::runtime_error("Demo CSV files are missing under examples/demo");
	}
	owner.BeginTransaction();
	try {
		auto schema = owner.Query("CREATE TABLE products(id INTEGER PRIMARY KEY,name VARCHAR,brand VARCHAR,category VARCHAR,size VARCHAR,unit VARCHAR,country_code VARCHAR); CREATE TABLE prices(id INTEGER PRIMARY KEY,product_id INTEGER,price DECIMAL(18,2),day DATE)");
		if (schema->HasError()) schema->ThrowError();
		auto a = owner.Query("INSERT INTO products SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', columns={'id':'INTEGER','name':'VARCHAR','brand':'VARCHAR','category':'VARCHAR','size':'VARCHAR','unit':'VARCHAR','country_code':'VARCHAR'})", duckdb::Value(products.string()));
		if (a->HasError()) a->ThrowError();
		auto b = owner.Query("INSERT INTO prices SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', columns={'id':'INTEGER','product_id':'INTEGER','price':'DECIMAL(18,2)','day':'DATE'})", duckdb::Value(prices.string()));
		if (b->HasError()) b->ThrowError();
		auto valid = owner.Query("SELECT (SELECT count(*) FROM products)=46462 AND (SELECT count(*) FROM prices)=46462 AND NOT EXISTS(SELECT 1 FROM products WHERE country_code NOT IN ('AR','ES')) AND NOT EXISTS(SELECT 1 FROM prices p LEFT JOIN products x ON p.product_id=x.id WHERE x.id IS NULL)");
		if (valid->HasError()) valid->ThrowError();
		if (!valid->GetValue<bool>(0, 0)) throw std::runtime_error("Demo CSV integrity check failed");
		owner.Commit();
	} catch (...) { owner.Rollback(); throw; }
}

void Serve(const Options &options, const std::string &token) {
	Descriptor descriptor(options.state);
	fs::create_directories(options.database.parent_path());
	const std::string endpoint = "quack:127.0.0.1:" + std::to_string(options.port);
	{
		duckdb::DBConfig config;
		config.SetOptionByName(duckdb::Identifier("temp_directory"), duckdb::Value((options.database.parent_path() / "native-spill").string()));
		config.SetOptionByName(duckdb::Identifier("threads"), duckdb::Value::BIGINT(2));
		duckdb::DuckDB database(options.database.string(), &config);
		duckdb::Connection owner(database);
		if (options.init_demo) InitDemo(owner, options.root);
		bool active = false;
		try {
			auto builtins = owner.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
			if (builtins->HasError()) builtins->ThrowError();
			if (builtins->GetValue<int64_t>(0, 0) != 2) throw std::runtime_error("This server requires the integrated native DuckDB build");
			Query(owner, "CALL quack_serve(?, token=?)", duckdb::Value(endpoint), duckdb::Value(token));
			active = true;
			const std::string shared = "\"pid\":" + std::to_string(getpid()) +
			    ",\"database\":" + JsonString(options.database.string()) + ",\"builtin_extensions\":[\"httpfs\",\"quack\"]";
			descriptor.Write("{\"endpoint\":" + JsonString(endpoint) + ",\"token\":" + JsonString(token) + "," + shared +
			                 ",\"duckdb\":" + JsonString(duckdb::DuckDB::LibraryVersion()) + "}\n");
			std::cout << "{\"ready\":true,\"uri\":" << JsonString(endpoint) << "," << shared
			          << ",\"duckdb_version\":" << JsonString(duckdb::DuckDB::LibraryVersion())
			          << ",\"source_id\":" << JsonString(duckdb::DuckDB::SourceID()) << "}" << std::endl;
			while (!stop_requested) {
				struct timespec interval {0, 100000000};
				nanosleep(&interval, nullptr);
			}
			Query(owner, "CALL quack_stop(?)", duckdb::Value(endpoint));
			active = false;
		} catch (...) {
			if (active) {
				try { Query(owner, "CALL quack_stop(?)", duckdb::Value(endpoint)); } catch (...) {}
			}
			throw;
		}
	}
	// Quack sessions, Connection and DuckDB are closed before descriptor removal.
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
			size_t position;
			while ((position = message.find(token)) != std::string::npos) message.replace(position, token.size(), "[redacted]");
		}
		std::cerr << "duckonwire-server: " << message << '\n';
		return 1;
	}
}
