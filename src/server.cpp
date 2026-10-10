#include "duckdb.hpp"
#include "duckonwire/json.hpp"
#include "duckonwire/pgwire.hpp"
#include "duckonwire/access.hpp"
#include "initialize.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
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

struct Options {
	fs::path root, database, state;
	fs::path access_file, tls_cert, tls_key;
	std::string pg_host = "127.0.0.1";
	int port = 9495;
	int pg_port = 9496;
	int max_queries = 8;
	int query_timeout_ms = 30000;
	int threads = 2;
	bool init_demo = false;
	std::int64_t demo_prices = 0;
};

Options Parse(int argc, char **argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		std::string name(argv[i]);
		if (name == "--init-demo") { options.init_demo = true; continue; }
		if (name == "--help") {
			std::cout << "duckonwire-server [--root DIRECTORY] [--database PATH] [--port 9495] [--pg-port 9496] [--state PATH] [--init-demo] [--demo-prices ROWS] [--threads 2]\n"
			          << "Quack uses --port; PostgreSQL drivers use --pg-port (0 selects a free port).\n"
			          << "DUCKONWIRE_TOKEN sets local endpoint passwords; otherwise one is generated.\n"
			          << "Public: --pg-host ADDRESS --access-file PATH --tls-cert PATH --tls-key PATH\n"
			          << "PostgreSQL limits: --max-queries 8 --query-timeout-ms 30000\n"
			          << "Engine: --threads 2 (1..64 worker threads).\n"
			          << "Demo: --init-demo imports the catalog; --demo-prices 1000000 adds reproducible 30-day synthetic history instead of CSV prices (46462..10000000 rows).\n"
			          << "Access rules select one database name and users with read/write permissions; TLS and SCRAM required.\n";
			std::exit(0);
		}
		if (i + 1 == argc) throw std::runtime_error("Every option requires a value");
		std::string value(argv[++i]);
		if (name == "--root") options.root = value;
		else if (name == "--database") options.database = value;
		else if (name == "--state") options.state = value;
		else if (name == "--access-file") options.access_file = value;
		else if (name == "--tls-cert") options.tls_cert = value;
		else if (name == "--tls-key") options.tls_key = value;
		else if (name == "--pg-host") options.pg_host = value;
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
	options.database = OwnedPath(options.database.empty() ? fs::path("data/demo.duckdb") : options.database,
	                             options.root);
	options.state = OwnedPath(options.state.empty() ? fs::path(".runtime/native.json") : options.state, options.root);
	if (options.database == options.state) throw std::runtime_error("Database and descriptor paths must differ");
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
		for (const auto &sensitive : {options.access_file, options.tls_cert, options.tls_key}) {
			if (sensitive == options.database || sensitive == options.state)
				throw std::runtime_error("Configuration, TLS and database paths must differ");
		}
		if (options.access_file == options.tls_cert || options.access_file == options.tls_key || options.tls_cert == options.tls_key)
			throw std::runtime_error("Access rules and TLS files must use separate paths");
	}
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

void Serve(const Options &options, const std::string &token) {
	Descriptor descriptor(options.state);
	fs::create_directories(options.database.parent_path());
	const std::string endpoint = "quack:127.0.0.1:" + std::to_string(options.port);
	{
		duckdb::DBConfig config;
		config.SetOptionByName(duckdb::Identifier("temp_directory"), duckdb::Value((options.database.parent_path() / "native-spill").string()));
		config.SetOptionByName(duckdb::Identifier("threads"), duckdb::Value::BIGINT(options.threads));
		const bool protected_mode = !options.access_file.empty();
		duckonwire::AccessConfig rules;
		if (protected_mode) {
			rules = duckonwire::ReadAccessConfig(options.access_file.string());
			config.SetOptionByName(duckdb::Identifier("enable_external_access"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("allow_unsigned_extensions"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("allow_persistent_secrets"), duckdb::Value(false));
			config.SetOptionByName(duckdb::Identifier("secret_directory"), duckdb::Value((options.root / ".runtime/engine-secrets").string()));
			config.SetOptionByName(duckdb::Identifier("memory_limit"), duckdb::Value("1GiB"));
			config.SetOptionByName(duckdb::Identifier("max_temp_directory_size"), duckdb::Value("1GiB"));
		}
		duckdb::DuckDB database(options.database.string(), &config);
		duckdb::Connection owner(database);
		if (options.init_demo) {
			if (protected_mode) throw std::runtime_error("Initialize the demo locally before enabling access rules");
			duckonwire::demo::Initialize(owner, options.root, options.demo_prices);
		}
		std::unique_ptr<duckonwire::PgWireServer> pg_owner;
		if (protected_mode) {
			duckonwire::PgWireOptions settings;
			settings.host = options.pg_host;
			settings.port = static_cast<std::uint16_t>(options.pg_port);
			settings.database_name = rules.database;
			settings.max_active_queries = static_cast<std::size_t>(options.max_queries);
			settings.query_timeout_ms = static_cast<std::uint32_t>(options.query_timeout_ms);
			settings.tls_cert = options.tls_cert.string();
			settings.tls_key = options.tls_key.string();
			for (const auto &rule : rules.users)
				settings.users.push_back({rule.name, rule.verifier, rule.read_only});
			pg_owner = std::make_unique<duckonwire::PgWireServer>(database, std::move(settings));
		} else {
			pg_owner = std::make_unique<duckonwire::PgWireServer>(database, options.pg_host, static_cast<std::uint16_t>(options.pg_port), token,
			    static_cast<std::size_t>(options.max_queries), static_cast<std::uint32_t>(options.query_timeout_ms));
		}
		auto &postgres = *pg_owner;
		bool active = false;
		try {
			if (!protected_mode) {
				auto builtins = owner.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
				if (builtins->HasError()) builtins->ThrowError();
				if (builtins->GetValue<int64_t>(0, 0) != 2) throw std::runtime_error("This server requires the integrated native DuckDB build");
			}
			Query(owner, "CALL quack_serve(?, token=?)", duckdb::Value(endpoint), duckdb::Value(token));
			active = true;
			if (protected_mode) {
				Query(owner, "SET TimeZone = 'UTC'");
				Query(owner, "SET lock_configuration = true");
			}
			postgres.Start();
			const std::string shared = "\"pid\":" + std::to_string(getpid()) +
			    ",\"pg_endpoint\":" + JsonString(postgres.Endpoint()) +
			    ",\"pg_port\":" + std::to_string(postgres.Port()) +
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
			postgres.Stop();
			Query(owner, "CALL quack_stop(?)", duckdb::Value(endpoint));
			active = false;
		} catch (...) {
			postgres.Stop();
			if (active) {
				try { Query(owner, "CALL quack_stop(?)", duckdb::Value(endpoint)); } catch (...) {}
			}
			throw;
		}
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
