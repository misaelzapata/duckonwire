#pragma once

#include "native_server.hpp"
#include "duckonwire/scram.hpp"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>

namespace quack_security_test {
using test_support::Require;
namespace fs = std::filesystem;

// Never propagate DuckDB's error text: ATTACH contains a credential literal.
template <class RESULT> inline void SafeCheck(const RESULT &result) {
	Require(bool(result) && !result->HasError(), "Native fixture query failed");
}

inline std::unique_ptr<duckdb::DuckDB> LocalEngine(const std::string &path = {}) {
	duckdb::DBConfig config;
	config.SetOptionByName("threads", duckdb::Value::BIGINT(2));
	return std::make_unique<duckdb::DuckDB>(path.empty() ? nullptr : path.c_str(), &config);
}

inline int64_t Scalar(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	SafeCheck(result);
	Require(result->RowCount() == 1 && result->ColumnCount() == 1, "Native scalar shape changed");
	return result->template GetValue<int64_t>(0, 0);
}

template <class RESULT> inline void AuthorizationDenied(const RESULT &result) {
	Require(result && result->HasError(), "Restricted native SQL succeeded");
	const auto error = result->GetError();
	Require(error.find("read-only") != std::string::npos || error.find("not allowed") != std::string::npos ||
	        error.find("protected") != std::string::npos || error.find("permission") != std::string::npos ||
	        error.find("disabled") != std::string::npos,
	        "Native SQL failed without an authorization denial");
}

inline void Denied(duckdb::Connection &connection, const std::string &sql) {
	AuthorizationDenied(connection.Query(sql));
}

inline std::string RandomPassword() {
	unsigned char bytes[24];
	Require(RAND_bytes(bytes, sizeof(bytes)) == 1, "Cannot generate owned fixture credentials");
	static const char hex[] = "0123456789abcdef";
	std::string result;
	for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
	// Exercise SQL quoting and splitting the account credential at its first colon.
	return result + ":'fixture";
}

inline void Certificate(const fs::path &certificate, const fs::path &private_key) {
	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
	    EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
	Require(generator && EVP_PKEY_keygen_init(generator.get()) > 0 &&
	        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) > 0, "Cannot initialize fixture key");
	EVP_PKEY *raw = nullptr;
	Require(EVP_PKEY_keygen(generator.get(), &raw) > 0, "Cannot generate fixture key");
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
	std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
	Require(cert && X509_set_version(cert.get(), 2) && ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) &&
	        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) &&
	        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) && X509_set_pubkey(cert.get(), key.get()),
	        "Cannot initialize fixture certificate");
	auto name = X509_get_subject_name(cert.get());
	Require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	        reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0) &&
	        X509_set_issuer_name(cert.get(), name), "Cannot name fixture certificate");
	X509V3_CTX context {};
	X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
	for (auto item : std::vector<std::pair<int, const char *>> {{NID_basic_constraints, "critical,CA:TRUE"},
	        {NID_subject_alt_name, "DNS:localhost"}, {NID_ext_key_usage, "serverAuth"}}) {
		std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
		    X509V3_EXT_conf_nid(nullptr, &context, item.first, const_cast<char *>(item.second)), X509_EXTENSION_free);
		Require(extension && X509_add_ext(cert.get(), extension.get(), -1), "Cannot extend fixture certificate");
	}
	Require(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "Cannot sign fixture certificate");
	std::unique_ptr<FILE, decltype(&fclose)> certificate_file(fopen(certificate.c_str(), "wb"), fclose);
	std::unique_ptr<FILE, decltype(&fclose)> key_file(fopen(private_key.c_str(), "wb"), fclose);
	Require(certificate_file && key_file && chmod(private_key.c_str(), 0600) == 0 &&
	        PEM_write_X509(certificate_file.get(), cert.get()) &&
	        PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr),
	        "Cannot persist private fixture TLS files");
}

class Client {
public:
	Client() : database(LocalEngine()), connection(std::make_unique<duckdb::Connection>(*database)) {}
	void Attach(const std::string &endpoint, const std::string &credential) {
		SafeCheck(connection->Query("ATTACH " + test_support::Literal(endpoint) +
		    " AS remote (TOKEN " + test_support::Literal(credential) + ")"));
	}
	void Connect() { SafeCheck(connection->Query("CONNECT remote")); }
	std::unique_ptr<duckdb::DuckDB> database;
	std::unique_ptr<duckdb::Connection> connection;
};

inline bool RejectCredential(const std::string &endpoint, const std::string &credential) {
	const auto denied = [](const std::string &message) {
		return message.find("Authentication failed") != std::string::npos ||
		       message.find("Quack request rejected") != std::string::npos ||
		       message.find("Quack connection rejected") != std::string::npos;
	};
	try {
		Client client;
		auto result = client.connection->Query("ATTACH " + test_support::Literal(endpoint) +
		    " AS remote (TOKEN " + test_support::Literal(credential) + ")");
		return result && result->HasError() && denied(result->GetError());
	} catch (const std::exception &error) { return denied(error.what()); }
	catch (...) { return false; }
}

class Fixture {
public:
	Fixture(const fs::path &parent, const fs::path &executable,
	        const std::vector<std::string> &limits = {},
	        const std::function<void(duckdb::Connection &, bool)> &seed = {}, bool start = true)
	    : root(parent), primary_password(RandomPassword()), reporting_password(RandomPassword()), executable(executable) {
		const auto certificate = root.path() / "ca.pem", key = root.path() / "key.pem";
		const auto access_file = root.path() / "access.tsv";
		Certificate(certificate, key);
		{
			std::ofstream rules(access_file);
			for (const auto &name : {std::string("duckonwire"), std::string("reporting")}) {
				const auto verifier = duckonwire::MakeScramVerifier(Password(name), 4096);
				rules << "database\t" << name << '\n'
				      << "user\treader\tread\t" << verifier << '\n'
				      << "user\twriter\twrite\t" << verifier << '\n'
				      << "user\tmixed\t" << (name == "reporting" ? "write" : "read") << '\t' << verifier << '\n'
				      << "user\t" << (name == "reporting" ? "reporting_only" : "primary_only")
				      << "\tread\t" << verifier << '\n';
			}
			rules.close();
			Require(bool(rules) && chmod(access_file.c_str(), 0600) == 0, "Cannot persist private fixture rules");
		}
		Seed(root.path() / "primary.duckdb", false, seed);
		Seed(root.path() / "reporting.duckdb", true, seed);
		arguments = {"--database", "reporting=reporting.duckdb", "--access-file",
		    access_file.string(), "--tls-cert", certificate.string(), "--tls-key", key.string(),
		    "--auth-max-failures", "100", "--auth-window-seconds", "60", "--ban-seconds", "1",
		    "--max-connections-per-minute", "10000", "--max-requests-per-minute", "10000"};
		arguments.insert(arguments.end(), limits.begin(), limits.end());
		if (start) Start();
	}
	void Start() {
		server = std::make_unique<test_support::NativeServer>(root.path(), root.path() / "primary.duckdb",
		    executable, 0, false, arguments, 2);
	}
	void RequireStartupRejected(const std::string &payload) {
		// Rejected startup never needs these sockets. Keep the range owned until the
		// child exits so another concurrent fixture cannot occupy a quiet endpoint.
		std::unique_ptr<test_support::PortRangeReservation> reservation;
		for (unsigned attempt = 0; attempt < 128; ++attempt) {
			auto candidate = std::make_unique<test_support::PortRangeReservation>();
			if (candidate->Bind(2)) { reservation = std::move(candidate); break; }
		}
		Require(bool(reservation), "Cannot reserve rejected startup endpoint range");
		const auto port = reservation->port();
		std::string diagnostic;
		try {
			server = std::make_unique<test_support::NativeServer>(root.path(), root.path() / "primary.duckdb",
			    executable, port, false, arguments, 2);
			server->stop();
		} catch (const std::exception &error) { diagnostic = error.what(); }
		Require(diagnostic.find("Protected database contains an unsafe or unverifiable stored SQL definition") != std::string::npos,
		        "Unsafe stored definition was not rejected by startup policy");
		Require(diagnostic.find(payload) == std::string::npos && diagnostic.find(primary_password) == std::string::npos &&
		        diagnostic.find(reporting_password) == std::string::npos,
		        "Protected startup diagnostic exposed a private payload");
		Require(!fs::exists(root.path() / ("server-" + std::to_string(port) + ".json")),
		        "Rejected protected startup published a descriptor");
		for (int offset = 0; offset < 2; ++offset) {
			const auto socket_fd = socket(AF_INET, SOCK_STREAM, 0);
			Require(socket_fd >= 0, "Cannot probe rejected owned endpoint");
			const auto flags = fcntl(socket_fd, F_GETFL);
			if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
				close(socket_fd);
				throw std::runtime_error("Cannot bound rejected startup endpoint probe");
			}
			sockaddr_in address {};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(static_cast<std::uint16_t>(port + offset));
			const auto connected = connect(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
			bool live = connected == 0;
			if (connected != 0 && errno == EINPROGRESS) {
				pollfd descriptor {socket_fd, POLLOUT, 0};
				if (poll(&descriptor, 1, 100) > 0) {
					int socket_error = 0;
					socklen_t size = sizeof(socket_error);
					live = getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) == 0 && socket_error == 0;
				}
			}
			close(socket_fd);
			Require(!live, "Rejected protected startup left a live native endpoint");
		}
	}
	std::string Endpoint(const std::string &database = "duckonwire") const {
		if (database != "reporting") return server->endpoint();
		const auto &endpoint = server->endpoint();
		const auto colon = endpoint.rfind(':');
		Require(colon != std::string::npos, "Native fixture endpoint is malformed");
		return endpoint.substr(0, colon + 1) + std::to_string(std::stoi(endpoint.substr(colon + 1)) + 1);
	}
	const std::string &Password(const std::string &database = "duckonwire") const {
		return database == "reporting" ? reporting_password : primary_password;
	}
	std::string Credential(const std::string &user, const std::string &database = "duckonwire") const {
		return user + ':' + Password(database);
	}
	std::unique_ptr<Client> Login(const std::string &user = "reader", const std::string &database = "duckonwire",
	        bool connect = true) const {
		auto client = std::make_unique<Client>();
		client->Attach(Endpoint(database), Credential(user, database));
		if (connect) client->Connect();
		return client;
	}
	void Stop() {
		server->stop();
		Require(server->clean_shutdown(), "Owned protected native fixture did not stop gracefully");
	}
	test_support::TempRoot root;
	std::unique_ptr<test_support::NativeServer> server;
private:
	void Seed(const fs::path &path, bool reporting,
	        const std::function<void(duckdb::Connection &, bool)> &seed) {
		auto database = LocalEngine(path.string());
		duckdb::Connection connection(*database);
		SafeCheck(connection.Query("CREATE TABLE fixture_rows(id INTEGER PRIMARY KEY, stock INTEGER, label VARCHAR)"));
		SafeCheck(connection.Query("INSERT INTO fixture_rows VALUES (?, ?, ?)", duckdb::Value::INTEGER(1),
		    duckdb::Value::INTEGER(reporting ? 20 : 10), duckdb::Value(reporting ? "reporting" : "primary")));
		SafeCheck(connection.Query("CREATE TABLE " + std::string(reporting ? "reporting_marker" : "primary_marker") + "(id INTEGER)"));
		SafeCheck(connection.Query("CREATE SEQUENCE fixture_sequence START 1"));
		SafeCheck(connection.Query("CREATE TABLE concurrent_rows(id INTEGER PRIMARY KEY, stock INTEGER)"));
		if (seed) seed(connection, reporting);
	}
	const std::string primary_password, reporting_password;
	const fs::path executable;
	std::vector<std::string> arguments;
};

inline void WaitForBanExpiry() { std::this_thread::sleep_for(std::chrono::milliseconds(1200)); }

} // namespace quack_security_test
