#include "duckonwire/json.hpp"
#include "duckonwire/scram.hpp"
#include "native_server.hpp"

#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <fstream>
#include <map>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

namespace {
using Bytes = std::vector<unsigned char>;
using test_support::Require;

void U16(Bytes &out, std::uint16_t value) {
	out.push_back(static_cast<unsigned char>(value >> 8));
	out.push_back(static_cast<unsigned char>(value));
}
void U32(Bytes &out, std::uint32_t value) {
	for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>(value >> shift));
}
std::uint32_t Read32(const Bytes &in, std::size_t at) {
	Require(at + 4 <= in.size(), "Truncated backend integer");
	std::uint32_t value = 0;
	for (int i = 0; i < 4; ++i) value = (value << 8) | in[at + i];
	return value;
}
void String(Bytes &out, const std::string &value) {
	out.insert(out.end(), value.begin(), value.end()); out.push_back(0);
}
struct Reply { char type; Bytes body; };
std::string ErrorCode(const Reply &reply) {
	if (reply.type != 'E') return {};
	std::size_t at = 0;
	while (at < reply.body.size() && reply.body[at]) {
		const auto field = reply.body[at++];
		auto end = std::find(reply.body.begin() + at, reply.body.end(), 0);
		Require(end != reply.body.end(), "Unterminated backend error field");
		std::string value(reply.body.begin() + at, end);
		at = static_cast<std::size_t>(end - reply.body.begin()) + 1;
		if (field == 'C') return value;
	}
	return {};
}
bool HasError(const std::vector<Reply> &replies, const std::string &code) {
	for (const auto &reply : replies) if (ErrorCode(reply) == code) return true;
	return false;
}
bool Contains(const std::vector<Reply> &replies, char type) {
	for (const auto &reply : replies) if (reply.type == type) return true;
	return false;
}
std::string Command(const std::vector<Reply> &replies) {
	std::string command;
	for (const auto &reply : replies) if (reply.type == 'C') {
		Require(!reply.body.empty() && reply.body.back() == 0, "Invalid CommandComplete string");
		command.assign(reply.body.begin(), reply.body.end() - 1);
	}
	return command;
}
std::vector<std::pair<std::string, std::uint32_t>> Description(const std::vector<Reply> &replies) {
	for (const auto &reply : replies) if (reply.type == 'T') {
		Require(reply.body.size() >= 2, "Truncated RowDescription");
		const auto columns = (unsigned(reply.body[0]) << 8) | reply.body[1]; std::size_t at = 2;
		std::vector<std::pair<std::string, std::uint32_t>> result;
		for (unsigned i = 0; i < columns; ++i) {
			auto end = std::find(reply.body.begin() + at, reply.body.end(), 0);
			Require(end != reply.body.end(), "Unterminated RowDescription name");
			std::string name(reply.body.begin() + at, end); at = static_cast<std::size_t>(end - reply.body.begin()) + 1;
			Require(at + 18 <= reply.body.size(), "Truncated RowDescription column fields");
			result.emplace_back(name, Read32(reply.body, at + 6)); at += 18;
		}
		Require(at == reply.body.size(), "Trailing RowDescription bytes"); return result;
	}
	return {};
}
std::vector<std::vector<std::optional<std::string>>> Rows(const std::vector<Reply> &replies) {
	std::vector<std::vector<std::optional<std::string>>> rows;
	for (const auto &reply : replies) {
		if (reply.type != 'D') continue;
		Require(reply.body.size() >= 2, "Truncated backend row");
		std::size_t at = 2;
		const auto columns = (unsigned(reply.body[0]) << 8) | reply.body[1];
		std::vector<std::optional<std::string>> row;
		for (unsigned i = 0; i < columns; ++i) {
			const auto length = static_cast<std::int32_t>(Read32(reply.body, at)); at += 4;
			if (length == -1) { row.emplace_back(); continue; }
			Require(length >= 0 && static_cast<std::size_t>(length) <= reply.body.size() - at, "Invalid backend field size");
			row.emplace_back(std::string(reply.body.begin() + at, reply.body.begin() + at + length)); at += length;
		}
		Require(at == reply.body.size(), "Trailing backend row bytes"); rows.push_back(std::move(row));
	}
	return rows;
}
char State(const std::vector<Reply> &replies) {
	Require(!replies.empty() && replies.back().type == 'Z' && replies.back().body.size() == 1, "Missing ReadyForQuery state");
	return static_cast<char>(replies.back().body[0]);
}

std::string Base64(const std::string &bytes) {
	std::string result(4 * ((bytes.size() + 2) / 3), '\0');
	EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()),
	                reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
	return result;
}
std::string Unbase64(const std::string &text) {
	std::string bytes(3 * (text.size() / 4), '\0');
	int size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
	                           reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
	Require(size >= 0, "Invalid fixture SCRAM base64");
	if (!text.empty() && text.back() == '=') --size;
	if (text.size() > 1 && text[text.size() - 2] == '=') --size;
	bytes.resize(static_cast<std::size_t>(size)); return bytes;
}
std::string Hmac(const std::string &key, const std::string &text) {
	std::string output(32, '\0'); unsigned size = 0;
	Require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
	             reinterpret_cast<const unsigned char *>(text.data()), text.size(),
	             reinterpret_cast<unsigned char *>(output.data()), &size) && size == 32, "Fixture HMAC failed");
	return output;
}
std::string Digest(const std::string &text) {
	std::string output(32, '\0'); unsigned size = 0;
	Require(EVP_Digest(text.data(), text.size(), reinterpret_cast<unsigned char *>(output.data()), &size, EVP_sha256(), nullptr) && size == 32,
	        "Fixture SHA256 failed"); return output;
}
std::pair<std::string, std::string> ScramProof(const std::string &password, const std::string &bare,
                                            const std::string &challenge) {
	std::map<char, std::string> attrs;
	for (std::size_t at = 0; at < challenge.size();) {
		auto end = challenge.find(',', at); if (end == std::string::npos) end = challenge.size();
		Require(end > at + 2 && challenge[at + 1] == '=', "Malformed fixture SCRAM challenge");
		attrs.emplace(challenge[at], challenge.substr(at + 2, end - at - 2)); at = end + 1;
	}
	auto salt = Unbase64(attrs.at('s')); std::string salted(32, '\0');
	Require(PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
	                         reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
	                         std::stoi(attrs.at('i')), EVP_sha256(), 32,
	                         reinterpret_cast<unsigned char *>(salted.data())) == 1, "Fixture PBKDF2 failed");
	auto client = Hmac(salted, "Client Key"); const auto final = "c=biws,r=" + attrs.at('r');
	const auto auth = bare + "," + challenge + "," + final; auto signature = Hmac(Digest(client), auth);
	for (std::size_t i = 0; i < client.size(); ++i) client[i] ^= signature[i];
	return {final + ",p=" + Base64(client), "v=" + Base64(Hmac(Hmac(salted, "Server Key"), auth))};
}

void FixtureCertificate(const std::filesystem::path &cert_path, const std::filesystem::path &key_path) {
	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
	Require(generator && EVP_PKEY_keygen_init(generator.get()) > 0 &&
	        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) > 0, "Fixture key setup failed");
	EVP_PKEY *raw = nullptr; Require(EVP_PKEY_keygen(generator.get(), &raw) > 0, "Fixture key generation failed");
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
	std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
	Require(cert && X509_set_version(cert.get(), 2) && ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) &&
	        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) && X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) &&
	        X509_set_pubkey(cert.get(), key.get()), "Fixture certificate setup failed");
	auto name = X509_get_subject_name(cert.get());
	Require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	        reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0) && X509_set_issuer_name(cert.get(), name),
	        "Fixture certificate name failed");
	X509V3_CTX context {}; X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
	for (auto item : std::vector<std::pair<int, const char *>> {{NID_basic_constraints, "critical,CA:TRUE"},
	        {NID_subject_alt_name, "DNS:localhost"}, {NID_ext_key_usage, "serverAuth"}}) {
		std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
		    X509V3_EXT_conf_nid(nullptr, &context, item.first, const_cast<char *>(item.second)), X509_EXTENSION_free);
		Require(extension && X509_add_ext(cert.get(), extension.get(), -1), "Fixture certificate extension failed");
	}
	Require(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "Fixture certificate signature failed");
	std::unique_ptr<FILE, decltype(&fclose)> cert_file(fopen(cert_path.c_str(), "wb"), fclose), key_file(fopen(key_path.c_str(), "wb"), fclose);
	Require(cert_file && key_file && chmod(key_path.c_str(), 0600) == 0 &&
	        PEM_write_X509(cert_file.get(), cert.get()) && PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr),
	        "Fixture certificate persistence failed");
}

class Wire {
public:
	explicit Wire(int port) {
		fd_ = socket(AF_INET, SOCK_STREAM, 0); Require(fd_ >= 0, "Cannot create protocol socket");
		timeval deadline {5, 0};
		setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
		setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
#ifdef SO_NOSIGPIPE
		int no_signal = 1; setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
		sockaddr_in address {}; address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
			close(fd_); fd_ = -1; throw std::runtime_error("Cannot connect to protocol fixture");
		}
	}
	Wire(const Wire &) = delete;
	Wire &operator=(const Wire &) = delete;
	~Wire() { Close(); }
	void Close() {
		if (tls_) { SSL_free(tls_); tls_ = nullptr; }
		if (tls_context_) { SSL_CTX_free(tls_context_); tls_context_ = nullptr; }
		if (fd_ >= 0) { shutdown(fd_, SHUT_RDWR); close(fd_); fd_ = -1; }
	}
	void Send(const Bytes &bytes) {
		std::size_t at = 0;
		while (at < bytes.size()) {
			if (tls_) {
				auto sent = SSL_write(tls_, bytes.data() + at, static_cast<int>(bytes.size() - at));
				Require(sent > 0, "TLS protocol send failed"); at += static_cast<std::size_t>(sent); continue;
			}
#ifdef MSG_NOSIGNAL
			auto sent = send(fd_, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
#else
			auto sent = send(fd_, bytes.data() + at, bytes.size() - at, 0);
#endif
			if (sent < 0 && errno == EINTR) continue;
			Require(sent > 0, "Protocol send failed or timed out"); at += static_cast<std::size_t>(sent);
		}
	}
	void Message(char type, const Bytes &body = {}) {
		Bytes frame {static_cast<unsigned char>(type)}; U32(frame, static_cast<std::uint32_t>(body.size() + 4));
		frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
	}
	bool Receive(void *output, std::size_t size) {
		auto bytes = static_cast<unsigned char *>(output); std::size_t at = 0;
		while (at < size) {
			if (tls_) {
				auto count = SSL_read(tls_, bytes + at, static_cast<int>(size - at));
				if (!count && !at) return false;
				Require(count > 0, "TLS protocol receive failed"); at += static_cast<std::size_t>(count); continue;
			}
			auto count = recv(fd_, bytes + at, size - at, 0);
			if (count < 0 && errno == EINTR) continue;
			if (!count && !at) return false;
			Require(count > 0, "Protocol receive failed or timed out"); at += static_cast<std::size_t>(count);
		}
		return true;
	}
	std::optional<Reply> Next() {
		unsigned char type; if (!Receive(&type, 1)) return {};
		Bytes header(4); Require(Receive(header.data(), 4), "Missing backend header");
		const auto length = Read32(header, 0);
		Require(length >= 4 && length <= 16 * 1024 * 1024, "Invalid backend frame length");
		Reply reply {static_cast<char>(type), Bytes(length - 4)};
		if (!reply.body.empty()) Require(Receive(reply.body.data(), reply.body.size()), "Missing backend frame body");
		return reply;
	}
	std::vector<Reply> Until(const std::string &terminal) {
		std::vector<Reply> replies;
		for (std::size_t i = 0; i < 50000; ++i) {
			auto reply = Next(); Require(bool(reply), "Backend disconnected before response completion");
			const auto type = reply->type; replies.push_back(std::move(*reply));
			if (terminal.find(type) != std::string::npos) return replies;
		}
		throw std::runtime_error("Backend response exceeds test message bound");
	}
	void Startup(const std::string &database = "duckonwire") {
		Bytes body; U32(body, 196608); String(body, "user"); String(body, "duckonwire");
		String(body, "database"); String(body, database); String(body, "client_encoding"); String(body, "UTF8"); body.push_back(0);
		Bytes frame; U32(frame, static_cast<std::uint32_t>(body.size() + 4)); frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
		auto auth = Next(); Require(auth && auth->type == 'R' && Read32(auth->body, 0) == 3, "Missing password challenge");
	}
	void Password(const std::string &password) { Bytes body; String(body, password); Message('p', body); }
	void Login(const std::string &password, const std::string &database = "duckonwire") {
		Startup(database); Password(password); const auto replies = Until("Z");
		Require(!Contains(replies, 'E') && State(replies) == 'I', "Protocol login failed");
		for (const auto &reply : replies) if (reply.type == 'K') {
			Require(reply.body.size() == 8, "Invalid backend cancellation key"); id = Read32(reply.body, 0); secret = Read32(reply.body, 4);
		}
		Require(id != 0, "Missing backend cancellation key");
	}
	void ProtectedLogin(const std::string &user, const std::string &password, const std::filesystem::path &cert,
	                    const std::string &database = "duckonwire") {
		ProtectedAuthentication(user, password, cert, database, false);
	}
	void RejectedProtectedLogin(const std::string &user, const std::string &password,
	                            const std::filesystem::path &cert, const std::string &database) {
		ProtectedAuthentication(user, password, cert, database, true);
	}
private:
	void ProtectedAuthentication(const std::string &user, const std::string &password,
	                             const std::filesystem::path &cert, const std::string &database, bool rejected) {
		Bytes request; U32(request, 8); U32(request, 80877103); Send(request);
		unsigned char answer = 0; Require(Receive(&answer, 1) && answer == 'S', "Protected TLS not accepted");
		tls_context_ = SSL_CTX_new(TLS_client_method());
		Require(tls_context_ && SSL_CTX_load_verify_locations(tls_context_, cert.c_str(), nullptr) &&
		        SSL_CTX_set_min_proto_version(tls_context_, TLS1_2_VERSION), "Fixture TLS trust setup failed");
		SSL_CTX_set_verify(tls_context_, SSL_VERIFY_PEER, nullptr); tls_ = SSL_new(tls_context_);
		Require(tls_ && SSL_set_fd(tls_, fd_) && SSL_set_tlsext_host_name(tls_, "localhost") &&
		        SSL_set1_host(tls_, "localhost") && SSL_connect(tls_) == 1 && SSL_get_verify_result(tls_) == X509_V_OK,
		        "Fixture TLS hostname verification failed");
		Bytes body; U32(body, 196608); String(body, "user"); String(body, user);
		String(body, "database"); String(body, database); body.push_back(0);
		Bytes frame; U32(frame, static_cast<std::uint32_t>(body.size() + 4)); frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
		auto auth = Next(); Require(auth && auth->type == 'R' && Read32(auth->body, 0) == 10, "Missing SCRAM mechanism challenge");
		std::string random(18, '\0'); Require(RAND_bytes(reinterpret_cast<unsigned char *>(random.data()), static_cast<int>(random.size())) == 1,
		                                        "Fixture nonce generation failed");
		const auto bare = "n=,r=" + Base64(random), first = "n,," + bare;
		body.clear(); String(body, "SCRAM-SHA-256"); U32(body, static_cast<std::uint32_t>(first.size()));
		body.insert(body.end(), first.begin(), first.end()); Message('p', body);
		auto continuation = Next(); Require(continuation && continuation->type == 'R' && Read32(continuation->body, 0) == 11,
		                                    "Missing SCRAM continuation");
		const auto proof = ScramProof(password, bare, std::string(continuation->body.begin() + 4, continuation->body.end()));
		Message('p', Bytes(proof.first.begin(), proof.first.end()));
		auto final = Next();
		if (rejected) {
			Require(final && ErrorCode(*final) == "28P01", "Rejected database/user did not complete ordinary SCRAM authentication failure");
			Require(!Next(), "Rejected protected authentication left the socket open"); return;
		}
		Require(final && final->type == 'R' && Read32(final->body, 0) == 12 &&
		    std::string(final->body.begin() + 4, final->body.end()) == proof.second, "SCRAM server signature mismatch");
		const auto replies = Until("Z"); Require(!Contains(replies, 'E') && State(replies) == 'I', "Protected login failed");
		for (const auto &reply : replies) if (reply.type == 'K') { id = Read32(reply.body, 0); secret = Read32(reply.body, 4); }
		Require(id != 0, "Missing protected cancellation key");
	}
public:
	void QuerySend(const std::string &sql) { Bytes body; String(body, sql); Message('Q', body); }
	std::vector<Reply> Query(const std::string &sql) { QuerySend(sql); return Until("Z"); }
	void Parse(const std::string &name, const std::string &sql, const std::vector<std::uint32_t> &oids = {}) {
		Bytes body; String(body, name); String(body, sql); U16(body, static_cast<std::uint16_t>(oids.size()));
		for (auto oid : oids) U32(body, oid); Message('P', body);
	}
	void Bind(const std::string &portal, const std::string &statement,
	          const std::vector<std::optional<std::string>> &values = {}, bool binary_output = false, bool binary_input = false) {
		Bytes body; String(body, portal); String(body, statement); U16(body, binary_input ? 1 : 0);
		if (binary_input) U16(body, 1);
		U16(body, static_cast<std::uint16_t>(values.size()));
		for (const auto &value : values) {
			U32(body, value ? static_cast<std::uint32_t>(value->size()) : UINT32_MAX);
			if (value) body.insert(body.end(), value->begin(), value->end());
		}
		U16(body, binary_output ? 1 : 0); if (binary_output) U16(body, 1); Message('B', body);
	}
	void Execute(const std::string &portal, std::uint32_t maximum = 0) { Bytes body; String(body, portal); U32(body, maximum); Message('E', body); }
	void Describe(char kind, const std::string &name) { Bytes body {static_cast<unsigned char>(kind)}; String(body, name); Message('D', body); }
	void CloseObject(char kind, const std::string &name) { Bytes body {static_cast<unsigned char>(kind)}; String(body, name); Message('C', body); }
	std::uint32_t id = 0, secret = 0;
private:
	int fd_ = -1;
	SSL_CTX *tls_context_ = nullptr;
	SSL *tls_ = nullptr;
};

std::int64_t Scalar(Wire &wire, const std::string &sql) {
	const auto replies = wire.Query(sql);
	Require(!Contains(replies, 'E'), "Scalar query failed"); const auto rows = Rows(replies);
	Require(rows.size() == 1 && rows[0].size() == 1 && rows[0][0], "Scalar result shape mismatch");
	return std::stoll(*rows[0][0]);
}

class ProtectedFixture {
public:
	ProtectedFixture(const std::filesystem::path &parent, const std::filesystem::path &executable,
	                 std::uint32_t timeout_ms = 30000) : root(parent), cert(root.path() / "ca.pem") {
		const auto key = root.path() / "key.pem", access = root.path() / "access.tsv";
		FixtureCertificate(cert, key);
		{
			std::ofstream file(access);
			file << "database\tduckonwire\nuser\twriter\twrite\t" << duckonwire::MakeScramVerifier(password, 4096)
			     << "\nuser\treader\tread\t" << duckonwire::MakeScramVerifier(password, 4096) << '\n';
			Require(bool(file) && chmod(access.c_str(), 0600) == 0, "Cannot create protected fixture rules");
		}
		server = std::make_unique<test_support::NativeServer>(root.path(), root.path() / "protected.duckdb", executable,
		    0, false, std::vector<std::string> {"--access-file", access.string(), "--tls-cert", cert.string(),
		        "--tls-key", key.string(), "--query-timeout-ms", std::to_string(timeout_ms)});
	}
	std::unique_ptr<Wire> Client(const std::string &role = "writer") {
		auto wire = std::make_unique<Wire>(server->pg_port()); wire->ProtectedLogin(role, password, cert); return wire;
	}
	void Cancel(const Wire &wire) {
		Wire cancellation(server->pg_port()); Bytes request;
		U32(request, 16); U32(request, 80877102); U32(request, wire.id); U32(request, wire.secret);
		cancellation.Send(request); Require(!cancellation.Next(), "Protected cancellation socket remained open");
	}
	test_support::TempRoot root;
	std::filesystem::path cert;
	std::unique_ptr<test_support::NativeServer> server;
private:
	const std::string password = "fixture-only-password-not-a-real-credential";
};

class MultiDatabaseFixture {
public:
	MultiDatabaseFixture(const std::filesystem::path &parent, const std::filesystem::path &executable)
	    : root(parent), cert(root.path() / "ca.pem"), primary(root.path() / "primary.duckdb"),
	      reporting(root.path() / "data" / "reporting.duckdb"), executable_(executable) {
		const auto key = root.path() / "key.pem", access = root.path() / "access.tsv";
		FixtureCertificate(cert, key);
		{
			std::ofstream file(access);
			const auto primary_verifier = duckonwire::MakeScramVerifier(Password("duckonwire"), 4096);
			const auto reporting_verifier = duckonwire::MakeScramVerifier(Password("reporting"), 4096);
			file << "database\tduckonwire\nuser\twriter\twrite\t" << primary_verifier
			     << "\nuser\treader\tread\t" << primary_verifier
			     << "\nuser\tmixed\tread\t" << primary_verifier
			     << "\nuser\tprimary_only\tread\t" << primary_verifier
			     << "\ndatabase\treporting\nuser\twriter\twrite\t" << reporting_verifier
			     << "\nuser\treader\tread\t" << reporting_verifier
			     << "\nuser\tmixed\twrite\t" << reporting_verifier
			     << "\nuser\treporting_only\tread\t" << reporting_verifier << '\n';
			Require(bool(file) && chmod(access.c_str(), 0600) == 0, "Cannot create isolated multi-database rules");
		}
		Start();
	}
	void Start() {
		Require(!server || server->pid() <= 0, "Cannot restart a running multi-database fixture");
		server = std::make_unique<test_support::NativeServer>(root.path(), primary, executable_, 0, false,
		    std::vector<std::string> {"--database", "reporting=data/reporting.duckdb", "--access-file",
		        (root.path() / "access.tsv").string(), "--tls-cert", cert.string(), "--tls-key",
		        (root.path() / "key.pem").string(), "--auth-max-failures", "100"}, 2);
	}
	std::unique_ptr<Wire> Client(const std::string &database, const std::string &user = "writer") {
		auto wire = std::make_unique<Wire>(server->pg_port());
		wire->ProtectedLogin(user, Password(database), cert, database); return wire;
	}
	void Reject(const std::string &database, const std::string &user, const std::string &credential_database) {
		Wire wire(server->pg_port()); wire.RejectedProtectedLogin(user, Password(credential_database), cert, database);
	}
	std::string SecondaryEndpoint() const {
		const auto &endpoint = server->endpoint(); const auto colon = endpoint.rfind(':');
		Require(colon != std::string::npos, "Malformed multi-database native endpoint");
		return endpoint.substr(0, colon + 1) + std::to_string(std::stoi(endpoint.substr(colon + 1)) + 1);
	}
	std::string NativeCredential(const std::string &database) const { return "writer:" + Password(database); }
	test_support::TempRoot root;
	std::filesystem::path cert, primary, reporting;
	std::unique_ptr<test_support::NativeServer> server;
private:
	const std::string &Password(const std::string &database) const {
		return database == "reporting" ? reporting_password_ : primary_password_;
	}
	std::filesystem::path executable_;
	const std::string primary_password_ = "isolated-primary-fixture-only-password";
	const std::string reporting_password_ = "isolated-reporting-fixture-only-password";
};

std::string ValuesInsert(const std::string &table, std::size_t rows, std::size_t columns) {
	std::string sql = "INSERT INTO " + table + " VALUES ";
	for (std::size_t row = 0, parameter = 1; row < rows; ++row) {
		if (row) sql += ',';
		sql += '(';
		for (std::size_t column = 0; column < columns; ++column, ++parameter) {
			if (column) sql += ',';
			sql += '$' + std::to_string(parameter);
		}
		sql += ')';
	}
	return sql;
}

// Start actual socket operations together. Exceptions are retained until every
// thread has joined, so failed assertions cannot leave a fixture thread alive.
void Concurrent(std::size_t count, const std::function<void(std::size_t)> &operation,
                const std::function<void()> &during = {}) {
	std::atomic<std::size_t> ready {0}; std::atomic<bool> start {false};
	std::vector<std::string> errors(count); std::vector<std::thread> threads;
	try {
		for (std::size_t i = 0; i < count; ++i) threads.emplace_back([&, i] {
			++ready; while (!start.load()) std::this_thread::yield();
			try { operation(i); } catch (const std::exception &error) { errors[i] = error.what(); }
		});
		while (ready.load() < count) std::this_thread::yield();
		start.store(true); if (during) during();
	} catch (...) {
		start.store(true); for (auto &thread : threads) if (thread.joinable()) thread.join(); throw;
	}
	for (auto &thread : threads) thread.join();
	for (const auto &error : errors) Require(error.empty(), error);
}
} // namespace

int main(int argc, char **argv) {
	std::filesystem::path server_program, fixture_parent = std::filesystem::temp_directory_path();
	for (int i = 1; i < argc; ++i) {
		std::string option = argv[i];
		if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
		if (option == "--server") server_program = argv[++i];
		else if (option == "--fixture-parent") fixture_parent = argv[++i];
		else { std::cerr << "Unknown option\n"; return 2; }
	}
	std::vector<std::pair<std::string, std::string>> checks;
	std::vector<std::string> private_tokens;
	try {
		Require(!server_program.empty(), "--server is required");
		test_support::TempRoot fixture(fixture_parent);
		test_support::NativeServer server(fixture.path(), fixture.path() / "protocol.duckdb", server_program);
		private_tokens.push_back(server.token());
		auto client = [&] { auto wire = std::make_unique<Wire>(server.pg_port()); wire->Login(server.token()); return wire; };
		auto test = [&](const std::string &name, const std::function<void()> &body) {
			try { body(); checks.emplace_back(name, ""); }
			catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
		};
		test("password_failure_is_fatal_and_bounded", [&] {
			Wire bad(server.pg_port()); bad.Startup(); bad.Password("deliberately-wrong-password");
			auto reply = bad.Next(); Require(reply && ErrorCode(*reply) == "28P01", "Authentication SQLSTATE mismatch");
			Require(!bad.Next(), "Rejected authentication did not close socket");
		});
		test("ssl_declined_then_plain_startup", [&] {
			Wire wire(server.pg_port()); Bytes request; U32(request, 8); U32(request, 80877103); wire.Send(request);
			unsigned char denied = 0; Require(wire.Receive(&denied, 1) && denied == 'N', "SSL negotiation mismatch");
			wire.Login(server.token()); Require(Scalar(wire, "SELECT 42") == 42, "Query after SSL negotiation failed");
		});
		test("wire_version_identifies_protocol_without_changing_native_engine", [&] {
			auto wire = client();
			auto check_version = [&] {
				const auto replies = wire->Query("SELECT version()"); const auto rows = Rows(replies);
				Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0].size() == 1 && rows[0][0] &&
				        rows[0][0]->find("PostgreSQL 16.0") == 0 && rows[0][0]->find("DuckOnWire") != std::string::npos,
				        "version() did not identify PostgreSQL 16.0 wire compatibility and DuckOnWire");
			};
			check_version();
			Require(!Contains(wire->Query("DISCARD ALL"), 'E'), "Version fixture DISCARD ALL failed"); check_version();
			duckdb::DuckDB local(nullptr); duckdb::Connection native(local);
			test_support::Connect(native, server.endpoint(), server.token());
			auto result = native.Query("SELECT version()"); test_support::Check(result);
			Require(result->GetValue(0, 0).GetValue<std::string>() == duckdb::DuckDB::LibraryVersion(),
			        "Wire compatibility identity leaked into native Quack version()");
		});
		test("legacy_single_database_accepts_label_and_keeps_primary_route", [&] {
			auto primary = client();
			Require(!Contains(primary->Query("CREATE TABLE pgwire_legacy_label(id INTEGER)"), 'E') &&
			        !Contains(primary->Query("INSERT INTO pgwire_legacy_label VALUES(43)"), 'E'), "Legacy label fixture setup failed");
			Wire labelled(server.pg_port()); labelled.Login(server.token(), "a_client_label_not_a_new_database");
			Require(Scalar(labelled, "SELECT id FROM pgwire_legacy_label") == 43 &&
			        Scalar(*primary, "SELECT id FROM pgwire_legacy_label") == 43,
			        "A single-database legacy login label selected a different database");
		});
		test("network_copy_is_explicitly_unsupported_in_simple_and_extended_protocol", [&] {
			auto wire = client();
			Require(!Contains(wire->Query("CREATE TABLE pgwire_network_copy(id INTEGER)"), 'E'), "COPY fixture creation failed");
			const auto stdout_before = std::filesystem::exists("stdout"), upper_before = std::filesystem::exists("STDOUT");
			for (const auto &sql : {std::string("COPY (SELECT 87654321) TO STDOUT (FORMAT CSV)"),
			                       std::string("CoPy (SELECT 87654321) tO /* wire stream */ sTdOuT"),
			                       std::string("COPY pgwire_network_copy FROM STDIN (FORMAT CSV)"),
			                       std::string("EXPLAIN ANALYZE COPY pgwire_network_copy FROM STDIN (FORMAT CSV)"),
			                       std::string("PREPARE blocked_export AS COPY (SELECT 87654321) TO STDOUT"),
			                       std::string("PREPARE blocked_import AS COPY pgwire_network_copy FROM STDIN (FORMAT CSV)")}) {
				const auto simple = wire->Query(sql);
				Require(HasError(simple, "0A000") && State(simple) == 'I' && !Contains(simple, 'C') &&
				        !Contains(simple, 'H') && !Contains(simple, 'G') && !Contains(simple, 'd'),
				        "Simple network COPY silently succeeded or started an unsupported stream");
				wire->Parse("", sql); wire->Bind("", ""); wire->Execute(""); wire->Message('S');
				const auto prepared = wire->Until("Z");
				Require(HasError(prepared, "0A000") && State(prepared) == 'I' && !Contains(prepared, 'C') &&
				        !Contains(prepared, 'H') && !Contains(prepared, 'G') && !Contains(prepared, 'd'),
				        "Prepared network COPY silently succeeded or started an unsupported stream");
			}
			Require(std::filesystem::exists("stdout") == stdout_before && std::filesystem::exists("STDOUT") == upper_before,
			        "Network COPY created a local stdout file");
			Require(Scalar(*wire, "SELECT count(*) FROM pgwire_network_copy") == 0,
			        "Rejected COPY FROM modified the table or poisoned the session");
			const auto csv = fixture.path() / "local-copy.csv";
			const auto copied = wire->Query("COPY (SELECT 7 AS id) TO " + test_support::Literal(csv.string()) + " (HEADER)");
			Require(!Contains(copied, 'E') && Command(copied) == "COPY 1" && std::filesystem::is_regular_file(csv),
			        "Network COPY rejection disabled legitimate legacy local-file COPY");
			Require(Scalar(*wire, "SELECT sum(id) FROM read_csv(" + test_support::Literal(csv.string()) + ",header=true)") == 7,
			        "Local-file COPY changed exported values");
			Require(!Contains(wire->Query("CREATE TABLE stdin AS SELECT 8::INTEGER AS id"), 'E'), "COPY source-name fixture failed");
			const auto named_csv = fixture.path() / "named-source.csv";
			const auto named = wire->Query("COPY (SELECT * FROM stdin) TO " + test_support::Literal(named_csv.string()) + " (HEADER)");
			Require(!Contains(named, 'E') && Command(named) == "COPY 1" &&
			        Scalar(*wire, "SELECT sum(id) FROM read_csv(" + test_support::Literal(named_csv.string()) + ",header=true)") == 8,
			        "A table named stdin was mistaken for a network COPY stream");
		});
		test("malformed_frame_fails_closed_without_affecting_other_session", [&] {
			auto wire = client(); Bytes malformed {'Q'}; U32(malformed, 3); wire->Send(malformed);
			auto reply = wire->Next(); Require(reply && ErrorCode(*reply) == "08P01", "Malformed frame SQLSTATE mismatch");
			Require(!wire->Next(), "Malformed frame did not close socket");
			auto healthy = client(); Require(Scalar(*healthy, "SELECT 7") == 7, "Malformed frame affected server");
		});
		test("parser_error_and_sync_recovery", [&] {
			auto wire = client(); wire->Parse("bad", "SELCT 1"); wire->Parse("skipped", "SELECT 99"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(HasError(replies, "42601") && State(replies) == 'I', "Parse error recovery mismatch");
			Require(!Contains(replies, '1') && Scalar(*wire, "SELECT 8") == 8, "Messages after error were not skipped until Sync");
		});
		test("unsupported_parameter_oid_rejected_even_for_null", [&] {
			auto wire = client(); wire->Parse("unknown_oid", "SELECT $1::INTEGER", {999999});
			wire->Bind("unknown_portal", "unknown_oid", {std::nullopt}); wire->Execute("unknown_portal"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(HasError(replies, "0A000") && !Contains(replies, 'D'), "Unsupported OID was accepted with NULL");
			Require(Scalar(*wire, "SELECT 9") == 9, "Unsupported OID poisoned session");
		});
		test("text_parameter_unicode_null_decimal_and_blob", [&] {
			auto wire = client(); wire->Parse("typed", "SELECT $1::VARCHAR,$2::INTEGER,$3::DECIMAL(18,2),$4::BLOB,$5::DECIMAL(18,4)", {25,23,1700,17,1700});
			wire->Bind("typed_portal", "typed", {std::string("Cafe 日本"),std::nullopt,std::string("19.99"),std::string("\\x0001ff"),std::string("1e-2")});
			wire->Describe('P', "typed_portal"); wire->Execute("typed_portal"); wire->Message('S');
			auto replies = wire->Until("Z"); const auto rows = Rows(replies);
			Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0].size() == 5, "Typed parameter query failed");
			Require(rows[0][0] == "Cafe 日本" && !rows[0][1] && rows[0][2] == "19.99" && rows[0][3] == "\\x0001ff" && rows[0][4] == "0.0100", "Typed parameter values changed, including exponent numeric");
			wire->Parse("uint32", "SELECT $1::UINTEGER AS max_unsigned", {26});
			wire->Bind("uint32", "uint32", {std::string(4, char(0xff))}, true, true);
			wire->Describe('P', "uint32"); wire->Execute("uint32"); wire->Message('S');
			const auto unsigned_replies = wire->Until("Z"); const auto unsigned_columns = Description(unsigned_replies); const auto unsigned_rows = Rows(unsigned_replies);
			Require(!Contains(unsigned_replies, 'E') && unsigned_columns.size() == 1 && unsigned_columns[0].second == 26, "UINTEGER result did not advertise PostgreSQL OID26");
			Require(unsigned_rows.size() == 1 && unsigned_rows[0].size() == 1 && unsigned_rows[0][0] && unsigned_rows[0][0]->size() == 4 && Read32(Bytes(unsigned_rows[0][0]->begin(), unsigned_rows[0][0]->end()), 0) == UINT32_MAX, "Binary OID26 lost uint32 maximum");
			for (const auto &reply : unsigned_replies) if (reply.type == 'T') {
				auto name_end = std::find(reply.body.begin() + 2, reply.body.end(), 0); const auto size_at = static_cast<std::size_t>(name_end - reply.body.begin()) + 11;
				Require(size_at + 2 <= reply.body.size() && reply.body[size_at] == 0 && reply.body[size_at + 1] == 4, "OID26 RowDescription type width is not four bytes");
			}
		});
		test("prepared_binary_pagination_15003_rows", [&] {
			auto wire = client(); Require(State(wire->Query("BEGIN")) == 'T', "BEGIN state mismatch");
			wire->Parse("paged", "SELECT i::INTEGER FROM range(15003) t(i)"); wire->Bind("page", "paged", {}, true);
			wire->Describe('P', "page"); wire->Execute("page", 3); wire->Message('H');
			auto replies = wire->Until("sCE"); Require(Contains(replies, 's') && !Contains(replies, 'E'), "Initial portal page was not suspended");
			std::size_t count = 0;
			auto consume = [&](const std::vector<Reply> &part) {
				for (const auto &row : Rows(part)) {
					Require(row.size() == 1 && row[0] && row[0]->size() == 4, "Binary integer row shape mismatch");
					Bytes bytes(row[0]->begin(), row[0]->end()); Require(Read32(bytes, 0) == count++, "Paging lost or repeated a row");
				}
			};
			consume(replies);
			for (int page = 0; page < 4 && !Contains(replies, 'C'); ++page) {
				wire->Execute("page", 5000); wire->Message('H'); replies = wire->Until("sCE");
				Require(!Contains(replies, 'E'), "Resumed portal execution failed"); consume(replies);
			}
			Require(count == 15003 && Contains(replies, 'C'), "Portal did not complete exact row count");
			wire->Message('S'); Require(State(wire->Until("Z")) == 'T', "Sync closed explicit transaction");
			Require(State(wire->Query("COMMIT")) == 'I', "COMMIT state mismatch");
		});
		test("close_statement_closes_associated_portals", [&] {
			auto wire = client(); wire->Query("BEGIN"); wire->Parse("to_close", "SELECT 1"); wire->Bind("associated", "to_close");
			wire->CloseObject('S', "to_close"); wire->Execute("associated"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(Contains(replies, '3') && HasError(replies, "34000") && !Contains(replies, 'D'), "Closed statement left its portal executable");
			wire->Query("ROLLBACK");
		});
		test("explicit_constraint_failure_requires_rollback", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_unique(id INTEGER PRIMARY KEY)"); wire->Query("INSERT INTO pgwire_review_unique VALUES (1)");
			wire->Query("BEGIN"); auto failed = wire->Query("INSERT INTO pgwire_review_unique VALUES (1)");
			Require(HasError(failed, "23505") && State(failed) == 'E', "Constraint error transaction state mismatch");
			Require(HasError(wire->Query("SELECT 1"), "25P02"), "Failed transaction allowed ordinary query");
			Require(State(wire->Query("ROLLBACK")) == 'I' && Scalar(*wire, "SELECT count(*) FROM pgwire_review_unique") == 1, "ROLLBACK failed recovery");
		});
		test("extended_implicit_transaction_commits_only_at_sync", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_pipeline(id INTEGER)"); auto writer = client();
			writer->Parse("pending", "INSERT INTO pgwire_review_pipeline VALUES (1)"); writer->Bind("pending", "pending"); writer->Execute("pending"); writer->Message('H');
			Require(!Contains(writer->Until("CE"), 'E'), "Pipeline INSERT failed");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline") == 0, "INSERT committed before Sync");
			writer->Message('S'); Require(State(writer->Until("Z")) == 'I', "Implicit Sync state mismatch");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline") == 1, "Sync did not commit pending INSERT");
		});
		test("extended_implicit_error_rolls_back_prior_commands", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_pipeline_error(id INTEGER)"); auto writer = client();
			writer->Parse("insert", "INSERT INTO pgwire_review_pipeline_error VALUES (1)"); writer->Bind("insert", "insert"); writer->Execute("insert");
			writer->Parse("fail", "SELECT * FROM pgwire_review_missing_table"); writer->Message('S');
			auto replies = writer->Until("Z"); Require(Contains(replies, 'E') && State(replies) == 'I', "Failed implicit pipeline did not recover at Sync");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline_error") == 0, "Failed pipeline committed earlier INSERT");
		});
		test("disconnect_rolls_back_pending_explicit_write", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_close(id INTEGER PRIMARY KEY)");
			{ auto writer = client(); writer->Query("BEGIN"); writer->Query("INSERT INTO pgwire_review_close VALUES (1)"); }
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_close") == 0, "Disconnected transaction became visible");
			Require(!Contains(admin->Query("INSERT INTO pgwire_review_close VALUES (1)"), 'E'), "Disconnected transaction retained primary-key write");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_close") == 1, "Reused key did not persist after disconnect rollback");
		});
		test("mvcc_reader_keeps_snapshot_while_writer_commits", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_snapshot(id INTEGER PRIMARY KEY,stock INTEGER)");
			admin->Query("INSERT INTO pgwire_review_snapshot VALUES (1,3)"); auto reader = client(); auto writer = client();
			Require(State(reader->Query("BEGIN")) == 'T' && Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 3, "Initial reader snapshot mismatch");
			writer->Query("BEGIN"); Require(!Contains(writer->Query("UPDATE pgwire_review_snapshot SET stock=4 WHERE id=1"), 'E'), "Independent writer was blocked");
			Require(State(writer->Query("COMMIT")) == 'I', "Writer COMMIT failed");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 3, "Committed write changed existing reader snapshot");
			reader->Query("COMMIT"); reader->Query("BEGIN");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 4, "New transaction did not see committed writer"); reader->Query("COMMIT");
		});
		test("same_row_conflict_preserves_reader_and_rollback_recovers", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_conflict(id INTEGER PRIMARY KEY,stock INTEGER)");
			admin->Query("INSERT INTO pgwire_review_conflict VALUES (1,3)"); auto first = client(); auto second = client(); auto reader = client();
			first->Query("BEGIN"); second->Query("BEGIN"); first->Query("UPDATE pgwire_review_conflict SET stock=4 WHERE id=1");
			auto conflict = second->Query("UPDATE pgwire_review_conflict SET stock=5 WHERE id=1");
			Require(HasError(conflict, "40001") && State(conflict) == 'E', "Same-row conflict SQLSTATE/state mismatch");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_conflict") == 3, "Pending conflicting write blocked or altered reader");
			Require(State(second->Query("ROLLBACK")) == 'I', "Conflicted transaction did not roll back"); first->Query("COMMIT");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_conflict") == 4 && Scalar(*second, "SELECT stock FROM pgwire_review_conflict") == 4, "Conflict changed committed stock or poisoned session");
		});
		test("simple_batch_begin_promotes_prior_implicit_work", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_simple_transaction(id INTEGER)");
			const auto replies = wire->Query("INSERT INTO pgwire_review_simple_transaction VALUES (1); BEGIN; INSERT INTO pgwire_review_simple_transaction VALUES (2); ROLLBACK");
			Require(!Contains(replies, 'E') && State(replies) == 'I', "Simple transaction-control batch failed");
			Require(Scalar(*wire, "SELECT count(*) FROM pgwire_review_simple_transaction") == 0, "Statement before BEGIN escaped batch rollback");
		});
		test("dml_command_tags_and_returning_row_counts", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_tags(id INTEGER PRIMARY KEY,stock INTEGER)");
			auto check = [&](const std::string &sql, const std::string &tag, std::size_t rows) {
				const auto replies = wire->Query(sql);
				Require(!Contains(replies, 'E') && Command(replies) == tag, "DML CommandComplete tag or count mismatch");
				Require(Rows(replies).size() == rows, "DML produced an unexpected result row count");
				if (!rows) Require(!Contains(replies, 'T'), "Non-RETURNING DML exposed internal Count column");
			};
			check("INSERT INTO pgwire_review_tags VALUES (1,3),(2,3),(3,3)", "INSERT 0 3", 0);
			check("UPDATE pgwire_review_tags SET stock=stock+1 WHERE id<=2", "UPDATE 2", 0);
			check("DELETE FROM pgwire_review_tags WHERE id=3", "DELETE 1", 0);
			check("INSERT INTO pgwire_review_tags VALUES (4,3),(5,3) RETURNING id", "INSERT 0 2", 2);
			check("UPDATE pgwire_review_tags SET stock=stock+1 WHERE id<=2 RETURNING id", "UPDATE 2", 2);
			check("DELETE FROM pgwire_review_tags WHERE id IN (4,5) RETURNING id", "DELETE 2", 2);
			wire->Parse("insert_tag", "INSERT INTO pgwire_review_tags VALUES ($1,3)", {23});
			wire->Bind("insert_tag", "insert_tag", {std::string("7")}); wire->Describe('P', "insert_tag"); wire->Execute("insert_tag"); wire->Message('S');
			const auto prepared = wire->Until("Z");
			Require(!Contains(prepared, 'E') && Contains(prepared, 'n') && !Contains(prepared, 'D') && Command(prepared) == "INSERT 0 1", "Prepared DML tag/NoData mismatch");
		});
		test("schema_changes_are_fresh_across_pgwire_and_quack", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_schema(id INTEGER)"); wire->Query("INSERT INTO pgwire_review_schema VALUES (1)");
			Require(Scalar(*wire, "SELECT count(*) FROM pg_range") == 0 && Scalar(*wire, "SELECT count(*) FROM pg_enum") == 0, "Wire catalog advertised unsupported range/enum codecs");
			duckdb::DuckDB local(nullptr); duckdb::Connection quack(local); test_support::Connect(quack, server.endpoint(), server.token());
			auto persistent_catalog = quack.Query("SELECT count(*) FROM duckdb_views() WHERE NOT internal AND NOT temporary AND view_name IN ('pg_type','pg_namespace','pg_range','pg_enum')"); test_support::Check(persistent_catalog);
			Require(persistent_catalog->GetValue(0, 0).GetValue<std::int64_t>() == 0, "Wire catalog views leaked into persistent database");
			test_support::Check(quack.Query("SELECT id FROM pgwire_review_schema"));
			Require(!Contains(wire->Query("ALTER TABLE pgwire_review_schema ADD COLUMN label VARCHAR DEFAULT 'ready'"), 'E'), "PGwire ALTER failed");
			auto label = quack.Query("SELECT label FROM pgwire_review_schema"); test_support::Check(label);
			Require(label->GetValue(0, 0).GetValue<std::string>() == "ready", "Quack retained stale schema after PGwire ALTER");
			test_support::Check(quack.Query("ALTER TABLE pgwire_review_schema ADD COLUMN stock INTEGER DEFAULT 7"));
			Require(Scalar(*wire, "SELECT stock FROM pgwire_review_schema") == 7, "PGwire retained stale schema after Quack ALTER");
		});
		test("bound_file_table_functions_describe_actual_schema", [&] {
			const auto csv = fixture.path() / "parameterized.csv"; const auto blob = fixture.path() / "parameterized.bin";
			{ std::ofstream file(csv); file << "id,name,stock\n1,Cafe,3\n2,Arroz,10\n"; Require(bool(file), "Cannot create owned CSV fixture"); }
			{ std::ofstream file(blob, std::ios::binary); const char bytes[] {'\0','\1',char(0xff)}; file.write(bytes, 3); Require(bool(file), "Cannot create owned blob fixture"); }
			auto wire = client(); wire->Parse("csv_schema", "SELECT * FROM read_csv($1::VARCHAR, header=true)", {25});
			wire->Bind("csv_schema", "csv_schema", {csv.string()}); wire->Describe('P', "csv_schema"); wire->Execute("csv_schema"); wire->Message('S');
			const auto csv_replies = wire->Until("Z"); const auto columns = Description(csv_replies); const auto csv_rows = Rows(csv_replies);
			Require(!Contains(csv_replies, 'E') && columns.size() == 3 && columns[0].first == "id" && columns[1].first == "name" && columns[2].first == "stock", "Bound CSV Describe returned stale or unknown columns");
			Require(csv_rows.size() == 2 && csv_rows[0].size() == 3 && csv_rows[0][0] == "1" && csv_rows[0][1] == "Cafe" && csv_rows[0][2] == "3" && csv_rows[1][0] == "2" && csv_rows[1][1] == "Arroz" && csv_rows[1][2] == "10", "Bound CSV Execute values mismatch");
			wire->Parse("blob_schema", "SELECT content,size FROM read_blob($1::VARCHAR)", {25});
			wire->Bind("blob_schema", "blob_schema", {blob.string()}); wire->Describe('P', "blob_schema"); wire->Execute("blob_schema"); wire->Message('S');
			const auto blob_replies = wire->Until("Z"); const auto blob_columns = Description(blob_replies); const auto blob_rows = Rows(blob_replies);
			Require(!Contains(blob_replies, 'E') && blob_columns.size() == 2 && blob_columns[0].first == "content" && blob_columns[0].second == 17 && blob_columns[1].first == "size", "Bound blob Describe returned stale or unknown columns");
			Require(blob_rows.size() == 1 && blob_rows[0].size() == 2 && blob_rows[0][0] == "\\x0001ff" && blob_rows[0][1] == "3", "Bound blob Execute values mismatch");
		});
		test("describe_only_insert_does_not_execute_or_end_transaction", [&] {
			auto observer = client(); observer->Query("CREATE TABLE pgwire_review_describe(id INTEGER PRIMARY KEY)"); auto writer = client();
			writer->Parse("never_execute", "INSERT INTO pgwire_review_describe VALUES ($1) RETURNING id", {23});
			writer->Bind("never_execute", "never_execute", {std::string("1")}); writer->Describe('P', "never_execute"); writer->Message('S');
			const auto no_execute = writer->Until("Z");
			Require(!Contains(no_execute, 'E') && Description(no_execute).size() == 1 && State(no_execute) == 'I', "Describe-only INSERT metadata or state mismatch");
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_review_describe") == 0, "Describe-only INSERT mutated autocommit database");
			writer->Query("BEGIN"); writer->Query("INSERT INTO pgwire_review_describe VALUES (2)");
			writer->Parse("pending_describe", "INSERT INTO pgwire_review_describe VALUES ($1) RETURNING id", {23});
			writer->Bind("pending_describe", "pending_describe", {std::string("3")}); writer->Describe('P', "pending_describe"); writer->Message('H');
			const auto described = writer->Until("TE"); Require(!Contains(described, 'E') && Description(described).size() == 1, "Explicit transaction Describe failed");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_review_describe") == 0, "Describe committed previous explicit transaction write");
			writer->CloseObject('P', "pending_describe"); writer->Message('S'); Require(State(writer->Until("Z")) == 'T', "Closing described portal ended explicit transaction");
			Require(Scalar(*writer, "SELECT count(*) FROM pgwire_review_describe") == 1, "Describe executed pending INSERT or discarded previous write");
			writer->Query("COMMIT"); Require(Scalar(*observer, "SELECT sum(id) FROM pgwire_review_describe") == 2, "Describe changed committed transaction contents");
		});
		test("statement_describe_declared_types_and_unresolved_fail_closed", [&] {
			auto wire = client(); wire->Parse("declared_integer", "SELECT $1", {23}); wire->Describe('S', "declared_integer");
			wire->Bind("declared_integer", "declared_integer", {std::string("42")}, true); wire->Execute("declared_integer"); wire->Message('S');
			const auto integer_replies = wire->Until("Z"); const auto integer_columns = Description(integer_replies); const auto integer_rows = Rows(integer_replies);
			Require(!Contains(integer_replies, 'E') && integer_columns.size() == 1 && integer_columns[0].second == 23, "Statement Describe advertised wrong declared integer type");
			Require(integer_rows.size() == 1 && integer_rows[0].size() == 1 && integer_rows[0][0] && integer_rows[0][0]->size() == 4 && Read32(Bytes(integer_rows[0][0]->begin(), integer_rows[0][0]->end()), 0) == 42, "Statement-cached binary integer metadata disagrees with actual value");
			wire->Bind("declared_null", "declared_integer", {std::nullopt}, true); wire->Describe('P', "declared_null"); wire->Execute("declared_null"); wire->Message('S');
			const auto null_replies = wire->Until("Z"); const auto null_columns = Description(null_replies); const auto null_rows = Rows(null_replies);
			Require(!Contains(null_replies, 'E') && null_columns.size() == 1 && null_columns[0].second == 23 && null_rows.size() == 1 && null_rows[0].size() == 1 && !null_rows[0][0], "Declared NULL lost integer type or cached metadata compatibility");
			wire->Parse("unresolved", "SELECT $1"); wire->Describe('S', "unresolved"); wire->Message('S');
			const auto unresolved = wire->Until("Z"); Require(HasError(unresolved, "0A000") && !Contains(unresolved, 'T') && State(unresolved) == 'I', "Unresolved Statement Describe did not fail closed");
			wire->Parse("unbound_file", "SELECT * FROM read_blob($1::VARCHAR)", {25}); wire->Describe('S', "unbound_file"); wire->Message('S');
			const auto no_file = wire->Until("Z"); Require(HasError(no_file, "0A000") && !Contains(no_file, 'T') && State(no_file) == 'I', "Dynamic Statement Describe invented metadata without a bound filename");
			wire->Parse("explicit_cast", "SELECT $1::INTEGER"); wire->Describe('S', "explicit_cast"); wire->Bind("explicit_cast", "explicit_cast", {std::string("43")}, true); wire->Execute("explicit_cast"); wire->Message('S');
			const auto recovered = wire->Until("Z"); Require(!Contains(recovered, 'E') && Description(recovered).size() == 1 && Description(recovered)[0].second == 23 && Rows(recovered).size() == 1, "Explicit cast did not recover unresolved metadata session");
			wire->Parse("profiled", "EXPLAIN ANALYZE SELECT sum(i) FROM range($1::BIGINT) t(i)", {20});
			wire->Bind("profiled", "profiled", {std::string("1000")}); wire->Describe('P', "profiled"); wire->Execute("profiled"); wire->Message('S');
			const auto profiled = wire->Until("Z"); const auto profile_rows = Rows(profiled);
			Require(!Contains(profiled, 'E') && Description(profiled).size() == 2 && profile_rows.size() == 1 && profile_rows[0].size() == 2 && profile_rows[0][1], "Prepared EXPLAIN ANALYZE failed");
			Require(profile_rows[0][1]->find("Total Time:") != std::string::npos && profile_rows[0][1]->find("profiling is disabled") == std::string::npos, "Prepared EXPLAIN ANALYZE lost native profiling");
			Require(State(profiled) == 'I' && Scalar(*wire, "SELECT 44") == 44, "Prepared EXPLAIN ANALYZE left session unusable");
		});
		test("sql_cursor_requires_explicit_transaction", [&] {
			auto wire = client();
			const auto replies = wire->Query("DECLARE outside_cursor CURSOR FOR SELECT 1");
			Require(HasError(replies, "25P01") && State(replies) == 'I', "SQL cursor escaped transaction scope");
			Require(Scalar(*wire, "SELECT 11") == 11, "Rejected SQL cursor poisoned connection");
		});
		test("sql_cursor_forward_pagination_preserves_every_row", [&] {
			auto wire = client(); Require(State(wire->Query("BEGIN")) == 'T', "Cursor BEGIN failed");
			const auto declared = wire->Query("DECLARE page_cursor NO SCROLL CURSOR FOR SELECT i::INTEGER FROM range(15003) t(i)");
			Require(!Contains(declared, 'E') && Command(declared) == "DECLARE CURSOR" && !Contains(declared, 'D'), "Cursor declaration returned rows or failed");
			std::int64_t count = 0;
			for (unsigned page = 0; page < 17; ++page) {
				const auto fetched = wire->Query("FETCH FORWARD 1000 FROM page_cursor");
				Require(!Contains(fetched, 'E') && State(fetched) == 'T', "Cursor FETCH failed or ended transaction");
				const auto rows = Rows(fetched);
				Require(Command(fetched) == "FETCH " + std::to_string(rows.size()), "Cursor fetch command count differs from result");
				for (const auto &row : rows) Require(row.size() == 1 && row[0] && std::stoll(*row[0]) == count++, "Cursor lost, duplicated or reordered a row");
				if (rows.empty()) break;
			}
			Require(count == 15003, "SQL cursor failed complete pagination");
			Require(!Contains(wire->Query("CLOSE page_cursor"), 'E'), "Cursor close failed");
			Require(State(wire->Query("COMMIT")) == 'I', "Cursor COMMIT failed");
		});
		test("sql_cursor_identifier_case_and_duplicate_errors", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE \"Mixed Cursor\" CURSOR FOR SELECT 12"), 'E'), "Quoted cursor identifier rejected");
			const auto row = Rows(wire->Query("FETCH 1 FROM \"Mixed Cursor\""));
			Require(row.size() == 1 && row[0][0] == "12", "Quoted cursor result changed");
			Require(!Contains(wire->Query("DECLARE lower_cursor CURSOR FOR SELECT 13"), 'E'), "Unquoted cursor declaration failed");
			const auto lower = Rows(wire->Query("FETCH 1 FROM LOWER_CURSOR"));
			Require(lower.size() == 1 && lower[0].size() == 1 && lower[0][0] == "13", "Unquoted cursor identifier was not folded");
			const auto duplicate = wire->Query("DECLARE LOWER_CURSOR CURSOR FOR SELECT 99");
			Require(HasError(duplicate, "42P03") && State(duplicate) == 'E', "Duplicate cursor did not abort transaction with correct error");
			wire->Query("ROLLBACK"); Require(Scalar(*wire, "SELECT 14") == 14, "Duplicate cursor rollback failed");
		});
		test("sql_cursor_independent_positions_and_zero_fetch", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE first_cursor CURSOR FOR SELECT i::INTEGER FROM range(3) t(i)"), 'E'), "First cursor declaration failed");
			Require(!Contains(wire->Query("DECLARE second_cursor CURSOR FOR SELECT i::INTEGER FROM range(10,13) t(i)"), 'E'), "Second cursor declaration failed");
			const auto zero = wire->Query("FETCH 0 FROM first_cursor");
			Require(!Contains(zero, 'E') && Rows(zero).empty() && Description(zero).size() == 1 && Command(zero) == "FETCH 0", "Zero FETCH advanced or lost metadata");
			const auto first = Rows(wire->Query("FETCH 1 FROM first_cursor"));
			const auto second = Rows(wire->Query("FETCH 2 FROM second_cursor"));
			Require(first.size() == 1 && first[0][0] == "0" && second.size() == 2 && second[0][0] == "10" && second[1][0] == "11", "Cursor positions interfered");
			const auto remainder = Rows(wire->Query("FETCH ALL FROM first_cursor"));
			Require(remainder.size() == 2 && remainder[0][0] == "1" && remainder[1][0] == "2", "FETCH ALL lost remainder");
			wire->Query("ROLLBACK");
		});
		test("extended_cursor_declaration_binds_values_without_returning_rows", [&] {
			auto wire = client(); wire->Query("BEGIN");
			wire->Parse("declare_bound", "DECLARE bound_cursor CURSOR FOR SELECT $1::INTEGER AS id,$2::VARCHAR AS name,$3::DECIMAL(18,2) AS price", {23,25,1700});
			wire->Bind("declare_bound", "declare_bound", {std::string("15"),std::string("Coffee 'dark roast'"),std::string("19.25")});
			wire->Describe('P', "declare_bound"); wire->Execute("declare_bound"); wire->Message('S');
			const auto declared = wire->Until("Z");
			Require(!Contains(declared, 'E') && Contains(declared, 'n') && !Contains(declared, 'D') && Command(declared) == "DECLARE CURSOR" && State(declared) == 'T', "Extended DECLARE metadata or command mismatch");
			const auto fetched = wire->Query("FETCH 1 FROM bound_cursor"); const auto rows = Rows(fetched); const auto columns = Description(fetched);
			Require(!Contains(fetched, 'E') && rows.size() == 1 && rows[0].size() == 3 && rows[0][0] == "15" && rows[0][1] == "Coffee 'dark roast'" && rows[0][2] == "19.25", "Bound cursor values changed");
			Require(columns.size() == 3 && columns[0].second == 23 && columns[1].second == 25 && columns[2].second == 1700, "Bound cursor lost result types");
			wire->Query("COMMIT"); Require(Scalar(*wire, "SELECT 16") == 16, "Extended DECLARE sent extra ReadyForQuery frames");
		});
		test("extended_fetch_and_close_keep_protocol_synchronized", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE prepared_cursor CURSOR FOR SELECT i::INTEGER FROM range(2) t(i)"), 'E'), "Prepared control cursor setup failed");
			wire->Parse("fetch_cursor", "FETCH 1 FROM prepared_cursor"); wire->Bind("fetch_cursor", "fetch_cursor");
			wire->Describe('P', "fetch_cursor"); wire->Execute("fetch_cursor"); wire->Message('S');
			const auto fetched = wire->Until("Z"); const auto rows = Rows(fetched);
			Require(!Contains(fetched, 'E') && rows.size() == 1 && rows[0][0] == "0" && Command(fetched) == "FETCH 1" && State(fetched) == 'T', "Prepared FETCH failed");
			Require(Scalar(*wire, "SELECT 17") == 17, "Prepared FETCH emitted premature or duplicate ReadyForQuery");
			wire->Parse("close_cursor", "CLOSE prepared_cursor"); wire->Bind("close_cursor", "close_cursor");
			wire->Describe('P', "close_cursor"); wire->Execute("close_cursor"); wire->Message('S');
			const auto closed = wire->Until("Z");
			Require(!Contains(closed, 'E') && Contains(closed, 'n') && Command(closed) == "CLOSE CURSOR" && State(closed) == 'T', "Prepared CLOSE failed");
			Require(Scalar(*wire, "SELECT 18") == 18, "Prepared CLOSE left protocol frames unread"); wire->Query("COMMIT");
		});
		test("sql_cursors_are_removed_at_transaction_end", [&] {
			auto wire = client();
			for (const auto &ending : {std::string("COMMIT"),std::string("ROLLBACK")}) {
				wire->Query("BEGIN"); Require(!Contains(wire->Query("DECLARE end_cursor CURSOR FOR SELECT 19"), 'E'), "Transaction cursor setup failed");
				Require(State(wire->Query(ending)) == 'I', "Cursor transaction did not end"); wire->Query("BEGIN");
				const auto gone = wire->Query("FETCH 1 FROM end_cursor");
				Require(HasError(gone, "34000") && State(gone) == 'E', "Cursor survived transaction end"); wire->Query("ROLLBACK");
			}
			Require(Scalar(*wire, "SELECT 20") == 20, "Cursor cleanup poisoned session");
		});
		test("close_all_preserves_transaction_and_prepared_statements", [&] {
			auto observer = client(); auto wire = client();
			Require(!Contains(wire->Query("CREATE TABLE pgwire_cursor_close_all(id INTEGER)"), 'E'), "CLOSE ALL fixture setup failed");
			wire->Query("BEGIN"); wire->Query("INSERT INTO pgwire_cursor_close_all VALUES(21)");
			wire->Parse("still_prepared", "SELECT $1::INTEGER", {23}); wire->Message('S'); Require(!Contains(wire->Until("Z"), 'E'), "Prepared statement setup failed");
			Require(!Contains(wire->Query("DECLARE close_all_cursor CURSOR FOR SELECT 22"), 'E'), "CLOSE ALL cursor setup failed");
			const auto closed = wire->Query("CLOSE ALL");
			Require(!Contains(closed, 'E') && State(closed) == 'T', "CLOSE ALL reset connection or ended transaction");
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_cursor_close_all") == 0 && Scalar(*wire, "SELECT count(*) FROM pgwire_cursor_close_all") == 1, "CLOSE ALL committed or discarded pending write");
			wire->Bind("still_prepared", "still_prepared", {std::string("23")}); wire->Execute("still_prepared"); wire->Message('S');
			const auto preserved = wire->Until("Z"); const auto rows = Rows(preserved);
			Require(!Contains(preserved, 'E') && rows.size() == 1 && rows[0][0] == "23", "CLOSE ALL removed prepared statements");
			wire->Query("COMMIT"); Require(Scalar(*observer, "SELECT sum(id) FROM pgwire_cursor_close_all") == 21, "CLOSE ALL changed committed contents");
		});
		test("savepoints_do_not_fake_rollback_for_legacy_writers", [&] {
			auto wire = client(); wire->Query("BEGIN");
			const auto unsupported = wire->Query("SAVEPOINT writer_savepoint");
			Require(HasError(unsupported, "0A000") && State(unsupported) == 'E', "Writer savepoint was accepted without write rollback semantics");
			Require(State(wire->Query("ROLLBACK")) == 'I' && Scalar(*wire, "SELECT 24") == 24, "Rejected writer savepoint did not recover after rollback");
		});
		test("cancel_request_interrupts_query_and_session_recovers", [&] {
			auto wire = client(); wire->QuerySend("SELECT sum(sin(i::DOUBLE)) FROM range(1000000000) t(i)");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			Wire cancel(server.pg_port()); Bytes request; U32(request, 16); U32(request, 80877102); U32(request, wire->id); U32(request, wire->secret); cancel.Send(request);
			const auto replies = wire->Until("Z"); Require(HasError(replies, "57014") && State(replies) == 'I', "Cancel did not interrupt active query");
			Require(!cancel.Next() && Scalar(*wire, "SELECT 10") == 10, "Cancel socket or session recovery mismatch");
		});
		server.stop(); test("graceful_shutdown_with_protocol_sessions", [&] { Require(server.clean_shutdown(), "Native server required forced shutdown"); });
		ProtectedFixture protected_fixture(fixture_parent, server_program);
		private_tokens.push_back(protected_fixture.server->token());
		test("protected_network_copy_reports_unsupported_before_permission_policy", [&] {
			for (const auto &role : {std::string("writer"), std::string("reader")}) {
				auto wire = protected_fixture.Client(role);
				const auto version = Rows(wire->Query("SELECT version()"));
				Require(version.size() == 1 && version[0][0] && version[0][0]->find("PostgreSQL 16.0") == 0,
				        "Protected version() still advertises native engine as PostgreSQL wire version");
				for (const auto &sql : {std::string("COPY (SELECT 7) TO STDOUT"),
				                       std::string("COPY missing_copy_table FROM STDIN"),
				                       std::string("EXPLAIN ANALYZE COPY missing_copy_table FROM STDIN"),
				                       std::string("PREPARE blocked_export AS COPY (SELECT 7) TO STDOUT"),
				                       std::string("PREPARE blocked_import AS COPY missing_copy_table FROM STDIN")}) {
					const auto simple = wire->Query(sql);
					Require(HasError(simple, "0A000") && State(simple) == 'I' && !Contains(simple, 'C'),
					        "Protected network COPY was not rejected as an unsupported wire feature");
					wire->Parse("", sql); wire->Bind("", ""); wire->Execute(""); wire->Message('S');
					const auto extended = wire->Until("Z");
					Require(HasError(extended, "0A000") && State(extended) == 'I' && !Contains(extended, 'C'),
					        "Protected prepared COPY bypassed the unsupported-wire rejection");
				}
				Require(Scalar(*wire, "SELECT 44") == 44, "Protected COPY rejection poisoned the session");
			}
		});
		test("protected_concurrent_positional_values_and_reader_denial", [&] {
			auto observer = protected_fixture.Client();
			Require(!Contains(observer->Query("CREATE TABLE template_batch(id BIGINT PRIMARY KEY,amount DECIMAL(18,2),label VARCHAR)"), 'E'), "Protected table setup failed");
			const auto sql = ValuesInsert("template_batch", 1000, 3);
			std::vector<std::unique_ptr<Wire>> writers;
			for (unsigned i = 0; i < 4; ++i) writers.push_back(protected_fixture.Client());
			Concurrent(4, [&](std::size_t worker) {
				std::vector<std::optional<std::string>> values; std::vector<std::uint32_t> oids;
				for (unsigned row = 0; row < 1000; ++row) {
					values.emplace_back(std::to_string(worker * 1000 + row));
					values.emplace_back(std::to_string(row) + ".25"); values.emplace_back("Coffee '日本' " + std::to_string(worker));
					oids.insert(oids.end(), {20,1700,25});
				}
				auto &wire = *writers[worker]; wire.Parse("batch", sql, oids); wire.Bind("", "batch", values); wire.Execute(""); wire.Message('S');
				const auto replies = wire.Until("Z");
				Require(!Contains(replies, 'E') && State(replies) == 'I' && Command(replies) == "INSERT 0 1000", "Concurrent bound VALUES failed or lost affected count");
			});
			Require(Scalar(*observer, "SELECT count(*) FROM template_batch") == 4000 &&
			        Scalar(*observer, "SELECT sum(id) FROM template_batch") == 7998000 &&
			        Scalar(*observer, "SELECT sum(amount*100)::BIGINT FROM template_batch") == 199900000 &&
			        Scalar(*observer, "SELECT count(DISTINCT label) FROM template_batch") == 4, "Concurrent VALUES integrity differs");
			auto reader = protected_fixture.Client("reader"); reader->Parse("denied", sql); reader->Message('S');
			const auto denied = reader->Until("Z");
			Require(HasError(denied, "42501") && State(denied) == 'I' && Scalar(*reader, "SELECT count(*) FROM template_batch") == 4000,
			        "Retained INSERT template bypassed reader permissions or poisoned reader");
		});
		test("protected_template_uses_current_catalog_and_bound_types", [&] {
			const std::string sql = "INSERT INTO template_later VALUES ($1,$2)";
			auto missing = protected_fixture.Client(); missing->Parse("missing", sql, {20,25}); missing->Message('S');
			Require(HasError(missing->Until("Z"), "42P01"), "Missing table was accepted before CREATE");
			Require(!Contains(missing->Query("CREATE TABLE template_later(id BIGINT,label VARCHAR)"), 'E'), "Missing-table fixture CREATE failed");
			auto first = protected_fixture.Client(); first->Parse("fresh", sql, {20,25});
			first->Bind("", "fresh", {std::string("1234567890123"),std::nullopt}); first->Execute(""); first->Message('S');
			Require(!Contains(first->Until("Z"), 'E') && Scalar(*first, "SELECT id FROM template_later") == 1234567890123,
			        "Native binding did not recover after catalog CREATE");
			Require(!Contains(first->Query("ALTER TABLE template_later ALTER COLUMN id TYPE VARCHAR"), 'E'), "Template schema ALTER failed");
			auto second = protected_fixture.Client(); second->Parse("new_types", sql, {25,23});
			second->Bind("", "new_types", {std::string("current 'catalog'"),std::string("73")}); second->Execute(""); second->Message('S');
			Require(!Contains(second->Until("Z"), 'E'), "Cached syntax retained old prepared parameter types");
			const auto rows = Rows(second->Query("SELECT id,label FROM template_later ORDER BY id"));
			Require(rows.size() == 2 && rows[0][0] == "1234567890123" && !rows[0][1] &&
			        rows[1][0] == "current 'catalog'" && rows[1][1] == "73", "NULL, OIDs or altered schema changed bound values");
		});
		test("protected_template_replacement_preserves_native_semantics", [&] {
			auto wire = protected_fixture.Client();
			for (const auto &table : {std::string("template_a"),std::string("template_b")})
				Require(!Contains(wire->Query("CREATE TABLE " + table + "(id INTEGER PRIMARY KEY,label VARCHAR)"), 'E'), "Template replacement table setup failed");
			for (const auto &table : {std::string("template_a"),std::string("template_b"),std::string("template_a")}) {
				const int id = table == "template_b" ? 2 : int(Scalar(*wire, "SELECT count(*) FROM template_a")) + 1;
				wire->Parse("", "INSERT INTO " + table + " VALUES ($1,$2)", {23,25});
				wire->Bind("", "", {std::to_string(id),table}); wire->Execute(""); wire->Message('S');
				Require(!Contains(wire->Until("Z"), 'E'), "Replacing a syntax template changed destination table");
			}
			wire->Parse("bad", "INSERT INTO template_a VALUES ($1,", {23}); wire->Message('S');
			Require(HasError(wire->Until("Z"), "42601"), "Malformed template SQL did not fail natively");
			Require(State(wire->Query("BEGIN")) == 'T', "Template rollback BEGIN failed");
			wire->Parse("", "INSERT INTO template_b VALUES ($1,$2)", {23,25}); wire->Bind("", "", {std::string("3"),std::string("rolled back")});
			wire->Execute(""); wire->Message('S'); Require(!Contains(wire->Until("Z"), 'E'), "Bound transaction INSERT failed");
			Require(State(wire->Query("ROLLBACK")) == 'I' && Scalar(*wire, "SELECT count(*) FROM template_b") == 1, "Syntax template bypassed native rollback");
			wire->Parse("returning", "INSERT INTO template_b VALUES ($1::INTEGER,upper($2::VARCHAR)) RETURNING id,label", {23,25});
			wire->Bind("", "returning", {std::string("4"),std::string("native expression")}); wire->Execute(""); wire->Message('S');
			const auto returned = wire->Until("Z"); const auto rows = Rows(returned);
			Require(!Contains(returned, 'E') && rows.size() == 1 && rows[0][0] == "4" && rows[0][1] == "NATIVE EXPRESSION", "Uncached expression/RETURNING INSERT lost native behavior");
			Require(Scalar(*wire, "SELECT sum(id) FROM template_a") == 3 && Scalar(*wire, "SELECT sum(id) FROM template_b") == 6,
			        "Template replacement or syntax failure corrupted earlier rows");
		});
		test("protected_declare_uses_fresh_native_prepare_and_permissions", [&] {
			auto reader = protected_fixture.Client("reader"); Require(State(reader->Query("BEGIN")) == 'T', "Protected DECLARE BEGIN failed");
			const auto declared = reader->Query("DECLARE admitted_cursor CURSOR FOR SELECT id,label FROM template_a ORDER BY id");
			Require(!Contains(declared, 'E') && Command(declared) == "DECLARE CURSOR", "Protected native DECLARE failed");
			const auto fetched = Rows(reader->Query("FETCH ALL FROM admitted_cursor"));
			Require(fetched.size() == 2 && fetched[0][0] == "1" && fetched[1][0] == "2", "Protected cursor lost rows or ordering");
			reader->Query("CLOSE admitted_cursor"); reader->Query("COMMIT");
			auto writer = protected_fixture.Client();
			Require(!Contains(writer->Query("CREATE SEQUENCE template_sequence"), 'E'), "Protected sequence setup failed");
			reader->Parse("denied_function", "SELECT nextval('template_sequence')"); reader->Bind("", "denied_function"); reader->Execute(""); reader->Message('S');
			Require(HasError(reader->Until("Z"), "42501") && Scalar(*reader, "SELECT 42") == 42 &&
			        Scalar(*writer, "SELECT nextval('template_sequence')") == 1, "Fresh parse admission bypassed function permission policy");
		});
		test("protected_parse_contention_deadline_cancel_and_reuse", [&] {
			ProtectedFixture limited(fixture_parent, server_program, 5);
			std::vector<std::unique_ptr<Wire>> clients;
			for (unsigned i = 0; i < 3; ++i) clients.push_back(limited.Client());
			std::vector<std::vector<Reply>> replies(3);
			Concurrent(3, [&](std::size_t i) {
				clients[i]->Parse("cold", ValuesInsert("deadline_missing_" + std::to_string(i), 4000, 1)); clients[i]->Message('S');
				replies[i] = clients[i]->Until("Z");
			});
			for (const auto &reply : replies)
				Require((HasError(reply, "57014") || HasError(reply, "53300")) && State(reply) == 'I', "Cold parse or admission wait escaped the bounded deadline");
			// A syntax failure must release admission even under the small budget.
			clients[0]->Parse("syntax", "SELCT 1"); clients[0]->Message('S');
			Require(HasError(clients[0]->Until("Z"), "42601"), "Parse deadline left the admission slot occupied");
			limited.server->stop(); Require(limited.server->clean_shutdown(), "Deadline fixture could not stop gracefully");
			std::vector<std::unique_ptr<Wire>> cancelled;
			for (unsigned i = 0; i < 3; ++i) cancelled.push_back(protected_fixture.Client());
			std::vector<std::vector<Reply>> outcomes(3);
			Concurrent(3, [&](std::size_t i) {
				cancelled[i]->Parse("cancelled", ValuesInsert("cancel_missing_" + std::to_string(i), 10000, 1)); cancelled[i]->Message('S');
				outcomes[i] = cancelled[i]->Until("Z");
			}, [&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				for (const auto &wire : cancelled) protected_fixture.Cancel(*wire);
			});
			std::size_t interrupted = 0;
			for (std::size_t i = 0; i < outcomes.size(); ++i) {
				if (HasError(outcomes[i], "57014")) ++interrupted;
				Require(State(outcomes[i]) == 'I' && Scalar(*cancelled[i], "SELECT 43") == 43, "Cancelled native parse could not recover after Sync");
			}
			Require(interrupted > 0, "Cold parse cancellation did not reach an active request");
		});

		test("protected_select_template_uses_current_catalog_and_parameter_types", [&] {
			auto writer=protected_fixture.Client(); const std::string sql="SELECT id FROM select_template_later";
			Require(HasError(writer->Query(sql),"42P01"),"Unbound SELECT should fail before table creation");
			Require(!Contains(writer->Query("CREATE TABLE select_template_later(id INTEGER)"),'E') &&
			        !Contains(writer->Query("INSERT INTO select_template_later VALUES(7)"),'E'),"SELECT catalog fixture failed");
			Require(Scalar(*writer,sql)==7,"Cached syntax retained missing-table failure");
			Require(!Contains(writer->Query("ALTER TABLE select_template_later ALTER COLUMN id TYPE VARCHAR"),'E'),"SELECT schema alteration failed");
			const auto changed=writer->Query(sql);Require(!Contains(changed,'E') && Description(changed).size()==1 && Description(changed)[0].second==25 && Rows(changed)[0][0]=="7","Cached SELECT retained old result type");
			const std::string typed="SELECT $1 AS value,$2::VARCHAR AS label";
			for(auto oid:{23U,20U,25U}) {
				writer->Parse("",typed,{oid,25});writer->Bind("","",{oid==23?std::optional<std::string>("42"):oid==20?std::optional<std::string>("1234567890123"):std::nullopt,std::string("Coffee '日本'")});
				writer->Describe('P',"");writer->Execute("");writer->Message('S');const auto replies=writer->Until("Z");
				const auto rows=Rows(replies);const auto desc=Description(replies);
				Require(!Contains(replies,'E') && rows.size()==1 && rows[0][1]=="Coffee '日本'" && desc.size()==2 && desc[0].second==oid,"SELECT syntax cache reused bound values or OIDs");
				Require(oid==25?!rows[0][0]:rows[0][0]==(oid==23?"42":"1234567890123"),"SELECT parameters changed across private copies");
			}
		});
		test("protected_select_template_preserves_session_snapshot_and_authority", [&] {
			auto writer=protected_fixture.Client();auto other=protected_fixture.Client();auto reader=protected_fixture.Client("reader");
			for(auto *session:{writer.get(),other.get()})Require(!Contains(session->Query("CREATE TEMP TABLE select_context(id INTEGER)"),'E'),"Per-session temp fixture failed");
			Require(!Contains(writer->Query("INSERT INTO select_context VALUES(11)"),'E') && !Contains(other->Query("INSERT INTO select_context VALUES(22)"),'E'),"Temp fixture insert failed");
			Require(Scalar(*writer,"SELECT id FROM select_context")==11 && Scalar(*other,"SELECT id FROM select_context")==22 && Scalar(*writer,"SELECT id FROM select_context")==11,"Syntax template shared session catalog or values");
			writer->Query("CREATE TABLE select_snapshot(id INTEGER PRIMARY KEY,value INTEGER)");writer->Query("INSERT INTO select_snapshot VALUES(1,3)");
			const std::string snapshot="SELECT value FROM select_snapshot WHERE id=1";
			reader->Query("BEGIN");Require(Scalar(*reader,snapshot)==3,"SELECT reader initial snapshot failed");
			Require(!Contains(writer->Query("UPDATE select_snapshot SET value=4 WHERE id=1"),'E') && Scalar(*reader,snapshot)==3,"Cached SELECT lost its transaction snapshot");
			reader->Query("COMMIT");Require(Scalar(*reader,snapshot)==4,"SELECT template kept prior transaction snapshot");
			writer->Query("CREATE SEQUENCE select_guard_sequence");const std::string effect="SELECT nextval('select_guard_sequence')";
			Require(Scalar(*writer,effect)==1,"Writer side-effect fixture failed");
			reader->Parse("deny_cached",effect);reader->Bind("","deny_cached");reader->Execute("");reader->Message('S');
			Require(HasError(reader->Until("Z"),"42501") && Scalar(*writer,effect)==2 && Scalar(*reader,"SELECT 44")==44,"Copied SELECT syntax bypassed read-only binding or mutated sequence");
		});
		test("protected_select_template_eviction_and_uncached_shapes_preserve_results", [&] {
			auto reader=protected_fixture.Client("reader");
			for(int round=0;round<2;++round)for(int i=0;i<24;++i)Require(Scalar(*reader,"SELECT "+std::to_string(100+i)+" AS cached_value")==100+i,"SELECT template eviction changed its SQL key");
			Require(Scalar(*reader,"SELECT (SELECT 17) AS scalar_subquery")==17,"Uncached scalar subquery changed native behavior");
			const auto grouped=Rows(reader->Query("SELECT a,count(*) FROM (VALUES (1),(1),(2)) t(a) GROUP BY CUBE(a) ORDER BY a NULLS LAST"));
			Require(grouped.size()==3 && grouped[0][0]=="1" && grouped[0][1]=="2" && grouped[2][1]=="3","Uncached grouping containers changed native behavior");
			Require(Scalar(*reader,"SELECT 18 /*"+std::string(8500,'x')+"*/")==18,"Oversized SQL bypass changed native result");
			Require(HasError(reader->Query("SELECT missing_column FROM select_template_later"),"42703") && Scalar(*reader,"SELECT 19")==19,"SELECT cache error failed reusable autocommit recovery");
		});
		test("protected_shutdown_releases_parse_waiters", [&] {
			std::vector<std::unique_ptr<Wire>> clients;
			for (unsigned i = 0; i < 3; ++i) clients.push_back(protected_fixture.Client());
			Concurrent(3, [&](std::size_t i) {
				try {
					clients[i]->Parse("stopping", ValuesInsert("stop_missing_" + std::to_string(i), 10000, 1)); clients[i]->Message('S');
					while (auto reply = clients[i]->Next()) if (reply->type == 'Z') break;
				} catch (const std::exception &) {
					// Stop shuts down authenticated sockets; truncated TLS is an
					// expected client outcome, never a forced owner shutdown.
				}
			}, [&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(20)); protected_fixture.server->stop();
			});
			Require(protected_fixture.server->clean_shutdown(), "Active parsing or waiters required forced owner shutdown");
		});

		test("multiple_databases_reject_storage_aliases_duplicate_names_and_port_overflow", [&] {
			const int first = test_support::AvailablePortRange(2);
			test_support::PortRangeReservation occupied_neighbor;
			Require(occupied_neighbor.Bind(1, first + 1), "Cannot occupy the owned second fixture port");
			const int selected = test_support::AvailablePortRange(2, first);
			Require(selected != first && !(selected <= first + 1 && first + 1 < selected + 2),
			        "Port allocator selected a range containing an occupied second port");
			{
				test_support::PortRangeReservation first_reclaimed;
				Require(first_reclaimed.Bind(1, first), "Failed neighbor probe leaked its first socket");
			}
			{
				test_support::PortRangeReservation range_reclaimed;
				Require(range_reclaimed.Bind(2, selected), "Successful range probe retained its sockets");
			}
			bool final_port_was_free = false;
			{ test_support::PortRangeReservation last; final_port_was_free = last.Bind(1, 65535); }
			Require(test_support::AvailablePortRange(2, 65535) < 65535, "Native port range overflowed 65535");
			if (final_port_was_free) {
				test_support::PortRangeReservation final_reclaimed;
				Require(final_reclaimed.Bind(1, 65535), "Overflowed range probe leaked its first socket");
			}
			auto rejected = [&](const std::string &scenario, const std::vector<std::string> &arguments,
			                    const std::string &reason, int port = 0) {
				test_support::TempRoot invalid(fixture_parent); bool denied = false;
				const auto primary = invalid.path() / "primary.duckdb";
				try {
					test_support::NativeServer bad(invalid.path(), primary, server_program, port, false, arguments);
				} catch (const std::exception &error) {
					Require(std::string(error.what()).find(reason) != std::string::npos,
					        "Startup rejected " + scenario + " for an unexpected reason: " + error.what());
					denied = true;
				}
				Require(denied && !std::filesystem::exists(primary),
				        "Unsafe configuration " + scenario + " started an owner or created the primary database");
			};
			for (const auto &suffix : {std::string(".wal"), std::string(".wal.checkpoint"),
			                          std::string(".wal.recovery"), std::string(".tmp")}) {
				rejected("secondary database aliases primary " + suffix,
				    {"--database", "reporting=primary.duckdb" + suffix}, "paths must not overlap");
			}
			rejected("duplicate canonical database paths", {"--database", "reporting=./data/../primary.duckdb"}, "paths must not overlap");
			int descriptor_port = test_support::AvailablePort();
			while (descriptor_port == 65535) descriptor_port = test_support::AvailablePort();
			rejected("secondary database aliases server descriptor",
			    {"--database", "reporting=server-" + std::to_string(descriptor_port) + ".json"}, "paths must not overlap", descriptor_port);
			rejected("case-folded named database duplicates",
			    {"--database", "Reporting=second.duckdb", "--database", "reporting=third.duckdb"}, "unique, ignoring case");
			rejected("secondary Quack port overflows 65535", {"--database", "reporting=second.duckdb"}, "port range exceeds 65535", 65535);
		});
		MultiDatabaseFixture multiple(fixture_parent, server_program);
		private_tokens.push_back(multiple.server->token());
		test("multiple_databases_have_independent_native_catalogs_and_bound_results", [&] {
			auto primary = multiple.Client("duckonwire"), reporting = multiple.Client("reporting");
			Require(!Contains(primary->Query("CREATE TABLE md_shared(id INTEGER PRIMARY KEY,label VARCHAR)"), 'E') &&
			        !Contains(reporting->Query("CREATE TABLE md_shared(id INTEGER PRIMARY KEY,label INTEGER)"), 'E'),
			        "Same table name could not be created independently in both databases");
			const std::string insert = "INSERT INTO md_shared VALUES ($1,$2)";
			for (auto *wire : {primary.get(), reporting.get()}) {
				const bool first = wire == primary.get();
				wire->Parse("same_statement", insert, {23, first ? 25U : 23U});
				wire->Bind("same_portal", "same_statement", {std::string("1"), first ? "Primary '日本'" : "73"});
				wire->Execute("same_portal"); wire->Message('S');
				Require(!Contains(wire->Until("Z"), 'E'), "Identical INSERT syntax retained another database's types");
			}
			for (auto *wire : {primary.get(), reporting.get(), primary.get()}) {
				const bool first = wire == primary.get();
				wire->Parse("", "SELECT label FROM md_shared WHERE id=$1", {23});
				wire->Bind("", "", {std::string("1")}); wire->Describe('P', ""); wire->Execute(""); wire->Message('S');
				const auto replies = wire->Until("Z"); const auto rows = Rows(replies); const auto columns = Description(replies);
				Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0][0] == (first ? "Primary '日本'" : "73") &&
				        columns.size() == 1 && columns[0].second == (first ? 25U : 23U),
				        "Prepared SELECT shared bound data or result metadata between databases");
			}
			Require(!Contains(primary->Query("CREATE TABLE md_primary_only(id INTEGER); CREATE SCHEMA primary_only_schema; CREATE VIEW md_primary_view AS SELECT 11 AS value"), 'E') &&
			        !Contains(reporting->Query("CREATE TABLE md_reporting_only(id INTEGER); CREATE SCHEMA reporting_only_schema; CREATE VIEW md_reporting_view AS SELECT 22 AS value"), 'E'),
			        "Isolated metadata fixtures failed");
		});
		test("multiple_databases_authenticate_same_username_with_distinct_credentials", [&] {
			multiple.Reject("reporting", "writer", "duckonwire");
			multiple.Reject("duckonwire", "writer", "reporting");
			multiple.Reject("reporting", "primary_only", "duckonwire");
			multiple.Reject("duckonwire", "reporting_only", "reporting");
			multiple.Reject("not_a_published_database", "writer", "duckonwire");
			multiple.Reject("reporting", "unknown_user", "reporting");
			auto primary = multiple.Client("duckonwire", "primary_only"), reporting = multiple.Client("reporting", "reporting_only");
			Require(Scalar(*primary, "SELECT id FROM md_shared") == 1 && Scalar(*reporting, "SELECT label FROM md_shared") == 73,
			        "Rejected authentication altered valid routes or database membership");
		});
		test("multiple_databases_apply_read_write_permissions_per_database", [&] {
			auto primary = multiple.Client("duckonwire", "mixed"), reporting = multiple.Client("reporting", "mixed");
			Require(HasError(primary->Query("INSERT INTO md_shared VALUES(99,'denied')"), "42501"),
			        "Same user gained the other database's writer role");
			Require(!Contains(reporting->Query("INSERT INTO md_shared VALUES(99,99)"), 'E') &&
			        Scalar(*reporting, "SELECT label FROM md_shared WHERE id=99") == 99,
			        "Same user's reporting writer role was replaced by primary reader permissions");
			Require(!Contains(reporting->Query("DELETE FROM md_shared WHERE id=99"), 'E'), "Reporting writer cleanup was denied");
			for (const auto &database : {std::string("duckonwire"), std::string("reporting")}) {
				auto reader = multiple.Client(database, "reader");
				reader->Parse("denied", "INSERT INTO md_shared VALUES ($1,$2)", {23, database == "duckonwire" ? 25U : 23U});
				reader->Bind("denied", "denied", {std::string("100"), database == "duckonwire" ? "denied" : "100"});
				reader->Execute("denied"); reader->Message('S');
				Require(HasError(reader->Until("Z"), "42501") && Scalar(*reader, "SELECT count(*) FROM md_shared") == 1,
				        "Shared prepared syntax bypassed per-database read-only permissions");
			}
		});
		test("multiple_databases_read_while_both_native_instances_write", [&] {
			std::vector<std::unique_ptr<Wire>> sessions;
			sessions.push_back(multiple.Client("duckonwire")); sessions.push_back(multiple.Client("reporting"));
			sessions.push_back(multiple.Client("duckonwire", "reader")); sessions.push_back(multiple.Client("reporting", "reader"));
			std::atomic<unsigned> pending {0}, observed {0}, committed {0};
			auto wait = [](const std::atomic<unsigned> &count, const std::string &message) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
				while (count.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
				Require(count.load() == 2, message);
			};
			Concurrent(4, [&](std::size_t worker) {
				auto &wire = *sessions[worker];
				if (worker < 2) {
					Require(State(wire.Query("BEGIN")) == 'T', "Multi-database writer BEGIN failed");
					wire.Parse("pending", "INSERT INTO md_shared VALUES ($1,$2)", {23, worker == 0 ? 25U : 23U});
					wire.Bind("pending", "pending", {std::string("7"), worker == 0 ? "Primary committed" : "77"});
					wire.Execute("pending"); wire.Message('S');
					const auto replies = wire.Until("Z");
					Require(!Contains(replies, 'E') && State(replies) == 'T', "Writes to matching primary keys crossed database boundaries");
					++pending; wait(observed, "Readers could not query while both database writers were pending");
					Require(State(wire.Query("COMMIT")) == 'I', "Multi-database writer COMMIT failed"); ++committed;
				} else {
					wait(pending, "Both native writers did not reach pending transactions");
					Require(State(wire.Query("BEGIN")) == 'T' && Scalar(wire, "SELECT sum(id) FROM md_shared") == 1,
					        "Pending writes blocked a reader or leaked before COMMIT");
					++observed; wait(committed, "Independent database writers failed to commit");
					Require(Scalar(wire, "SELECT sum(id) FROM md_shared") == 1, "Commit changed an existing native reader snapshot");
					Require(State(wire.Query("COMMIT")) == 'I' && Scalar(wire, "SELECT sum(id) FROM md_shared") == 8,
					        "Fresh reader transaction did not see its own database's committed write");
				}
			});
		});
		test("multiple_databases_block_cross_catalog_data_and_ddl", [&] {
			auto primary = multiple.Client("duckonwire"), reporting = multiple.Client("reporting");
			auto catalog = [](Wire &wire) {
				const auto replies = wire.Query("SELECT current_database()"); const auto rows = Rows(replies);
				Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0].size() == 1 && rows[0][0], "Native catalog identity query failed");
				return rows[0][0].value();
			};
			const auto primary_catalog = catalog(*primary), reporting_catalog = catalog(*reporting);
			Require(primary_catalog != reporting_catalog, "Fixtures unexpectedly use the same native catalog name");
			for (auto *wire : {primary.get(), reporting.get()}) {
				const auto peer = (wire == primary.get() ? reporting_catalog : primary_catalog) + ".main.md_shared";
				for (const auto &sql : {"SELECT * FROM " + peer,
				                       "CREATE TABLE md_stolen AS SELECT * FROM " + peer,
				                       "CREATE VIEW md_stolen_view AS SELECT * FROM " + peer,
				                       "SELECT (SELECT count(*) FROM " + peer + ")",
				                       "EXPLAIN SELECT * FROM " + peer,
				                       "EXPLAIN ANALYZE SELECT * FROM " + peer,
				                       "INSERT INTO md_shared SELECT * FROM " + peer,
				                       "DELETE FROM " + peer,
				                       "ALTER TABLE " + peer + " ADD COLUMN stolen INTEGER",
				                       "DROP TABLE " + peer}) {
					const auto rejected = wire->Query(sql);
					Require(Contains(rejected, 'E') && State(rejected) == 'I' && !Contains(rejected, 'D'),
					        "Protected connection accessed or modified a different database's catalog");
				}
				Require(Scalar(*wire, "SELECT count(*) FROM md_shared") == 2 &&
				        Scalar(*wire, "SELECT count(*) FROM information_schema.tables WHERE table_name IN ('md_stolen','md_stolen_view')") == 0,
				        "Rejected cross-database query created local objects or altered selected data");
			}
		});
		test("multiple_databases_metadata_lists_only_selected_database", [&] {
			for (const auto &database : {std::string("duckonwire"), std::string("reporting")}) {
				auto reader = multiple.Client(database, "reader"); const bool primary = database == "duckonwire";
				const std::string peer = primary ? "reporting" : "primary";
				Require(Scalar(*reader, "SELECT count(*) FROM information_schema.tables WHERE table_name='md_" + peer + "_only'") == 0 &&
				        Scalar(*reader, "SELECT count(*) FROM duckdb_tables() WHERE table_name='md_" + peer + "_only'") == 0 &&
				        Scalar(*reader, "SELECT count(*) FROM duckdb_views() WHERE view_name='md_" + peer + "_view'") == 0 &&
				        Scalar(*reader, "SELECT count(*) FROM duckdb_schemas() WHERE schema_name='" + peer + "_only_schema'") == 0,
				        "Metadata exposed another database's tables, views or schemas");
				Require(Scalar(*reader, "SELECT count(*) FROM duckdb_databases() WHERE NOT internal") == 1 &&
				        Scalar(*reader, "SELECT count(*) FROM duckdb_databases() WHERE path=" +
				            test_support::Literal((primary ? multiple.reporting : multiple.primary).string())) == 0,
				        "Metadata exposed another published database file");
				Require(Scalar(*reader, "SELECT count(*) FROM duckdb_tables() WHERE NOT internal AND NOT temporary AND database_name<>current_database()") == 0 &&
				        Scalar(*reader, "SELECT count(*) FROM duckdb_views() WHERE NOT internal AND NOT temporary AND database_name<>current_database()") == 0,
				        "Native metadata contained persistent catalogs outside the selected database");
			}
		});
		test("multiple_databases_connection_and_pool_resets_preserve_route", [&] {
			const std::string pooled_reset = "SET SESSION AUTHORIZATION DEFAULT; RESET ALL; CLOSE ALL; UNLISTEN *; SELECT pg_advisory_unlock_all(); DISCARD SEQUENCES; DISCARD TEMP";
			for (const auto &database : {std::string("duckonwire"), std::string("reporting")}) {
				auto wire = multiple.Client(database); const bool primary = database == "duckonwire";
				auto check_route = [&] {
					const auto replies = wire->Query("SELECT label FROM md_shared WHERE id=1"); const auto rows = Rows(replies);
					Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0][0] == (primary ? "Primary '日本'" : "73"),
					        "Session reset reverted a client to another native database");
				};
				for (const auto &reset : {std::string("DEALLOCATE ALL"), std::string("DISCARD ALL")}) {
					Require(!Contains(wire->Query(reset), 'E'), "Multi-database session reset failed"); check_route();
				}
				wire->Parse("kept", "SELECT label FROM md_shared WHERE id=$1", {23}); wire->Message('S');
				Require(!Contains(wire->Until("Z"), 'E') && !Contains(wire->Query(pooled_reset), 'E'), "Pool reset fixture failed");
				wire->Bind("kept", "kept", {std::string("1")}); wire->Execute("kept"); wire->Message('S');
				const auto replies = wire->Until("Z"); const auto rows = Rows(replies);
				Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0][0] == (primary ? "Primary '日本'" : "73"),
				        "Reprepared statement after pool reset moved to the primary database"); check_route();
			}
		});
		test("multiple_databases_disconnect_rolls_back_each_native_transaction", [&] {
			for (const auto &database : {std::string("duckonwire"), std::string("reporting")}) {
				{
					auto writer = multiple.Client(database); Require(State(writer->Query("BEGIN")) == 'T', "Abandoned multi-database BEGIN failed");
					const auto sql = database == "duckonwire" ? "INSERT INTO md_shared VALUES(8,'rolled back')" : "INSERT INTO md_shared VALUES(8,88)";
					Require(!Contains(writer->Query(sql), 'E'), "Abandoned multi-database INSERT failed");
				}
				auto observer = multiple.Client(database);
				Require(Scalar(*observer, "SELECT count(*) FROM md_shared WHERE id=8") == 0 &&
				        !Contains(observer->Query(database == "duckonwire" ? "INSERT INTO md_shared VALUES(8,'reused')" : "INSERT INTO md_shared VALUES(8,88)"), 'E') &&
				        !Contains(observer->Query("DELETE FROM md_shared WHERE id=8"), 'E'),
				        "Closed connection committed pending data or retained a write lock in its database");
			}
		});
		test("multiple_databases_authenticated_native_attach_can_join_two_endpoints", [&] {
			duckdb::DuckDB local(nullptr); duckdb::Connection native(local);
			for (const auto &endpoint : {std::make_pair("primary_remote", multiple.server->endpoint()),
			                             std::make_pair("reporting_remote", multiple.SecondaryEndpoint())}) {
				const auto credential = multiple.NativeCredential(std::string(endpoint.first) == "primary_remote" ? "duckonwire" : "reporting");
				private_tokens.push_back(credential);
				test_support::Check(native.Query("ATTACH " + test_support::Literal(endpoint.second) + " AS " + endpoint.first +
				    " (TOKEN " + test_support::Literal(credential) + ")"));
			}
			auto joined = native.Query("SELECT p.id,p.label,r.label FROM primary_remote.main.md_shared p JOIN reporting_remote.main.md_shared r USING(id) ORDER BY id");
			test_support::Check(joined); const auto rows = test_support::FetchRows(*joined);
			Require(rows.size() == 2 && rows[0][0].GetValue<int>() == 1 && rows[0][1].GetValue<std::string>() == "Primary '日本'" &&
			        rows[0][2].GetValue<int>() == 73 && rows[1][0].GetValue<int>() == 7 && rows[1][2].GetValue<int>() == 77,
			        "Authenticated ATTACH failed to join distinct native catalogs on the same owner");
		});
		test("multiple_databases_graceful_restart_preserves_both_files_and_routes", [&] {
			multiple.server->stop(); Require(multiple.server->clean_shutdown(), "Multi-database owner required forced shutdown");
			Require(std::filesystem::is_regular_file(multiple.primary) && std::filesystem::is_regular_file(multiple.reporting),
			        "Multi-database owner did not create two persistent DuckDB files");
			multiple.Start();
			private_tokens.push_back(multiple.server->token());
			auto primary = multiple.Client("duckonwire"), reporting = multiple.Client("reporting");
			const auto primary_rows = Rows(primary->Query("SELECT id,label FROM md_shared ORDER BY id"));
			const auto reporting_rows = Rows(reporting->Query("SELECT id,label FROM md_shared ORDER BY id"));
			Require(primary_rows.size() == 2 && primary_rows[0][0] == "1" && primary_rows[0][1] == "Primary '日本'" &&
			        primary_rows[1][0] == "7" && primary_rows[1][1] == "Primary committed" &&
			        reporting_rows.size() == 2 && reporting_rows[0][0] == "1" && reporting_rows[0][1] == "73" &&
			        reporting_rows[1][0] == "7" && reporting_rows[1][1] == "77",
			        "Restart lost confirmed writes, revived abandoned writes or crossed database routes");
			primary.reset(); reporting.reset(); multiple.server->stop();
			Require(multiple.server->clean_shutdown(), "Restarted multi-database owner failed graceful shutdown");
		});
		test("multiple_databases_recover_confirmed_commits_after_owned_sigkill", [&] {
			multiple.Start(); private_tokens.push_back(multiple.server->token());
			auto primary = multiple.Client("duckonwire"), reporting = multiple.Client("reporting");
			Require(!Contains(primary->Query("INSERT INTO md_shared VALUES(9,'confirmed before crash')"), 'E') &&
			        !Contains(reporting->Query("INSERT INTO md_shared VALUES(9,99)"), 'E'), "Crash fixture did not confirm both commits");
			Require(State(primary->Query("BEGIN")) == 'T' && State(reporting->Query("BEGIN")) == 'T' &&
			        !Contains(primary->Query("INSERT INTO md_shared VALUES(10,'pending at crash')"), 'E') &&
			        !Contains(reporting->Query("INSERT INTO md_shared VALUES(10,100)"), 'E'), "Crash fixture pending transactions failed");
			Require(kill(multiple.server->pid(), SIGKILL) == 0, "Could not kill the owned isolated multi-database fixture");
			multiple.server->stop(); primary.reset(); reporting.reset();
			const int status = multiple.server->exit_status();
			Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
			        "Crash recovery did not actually follow SIGKILL");
			multiple.Start(); private_tokens.push_back(multiple.server->token());
			for (const auto &database : {std::string("duckonwire"), std::string("reporting")}) {
				auto recovered = multiple.Client(database);
				Require(Scalar(*recovered, "SELECT count(*) FROM md_shared") == 3 &&
				        Scalar(*recovered, "SELECT count(*) FROM md_shared WHERE id=9") == 1 &&
				        Scalar(*recovered, "SELECT count(*) FROM md_shared WHERE id=10") == 0,
				        "Crash recovery lost a confirmed commit or persisted a pending write in a database");
			}
			multiple.server->stop(); Require(multiple.server->clean_shutdown(), "Recovered multi-database owner failed graceful shutdown");
		});
	} catch (const std::exception &error) { checks.emplace_back("fixture_setup", error.what()); }
	bool passed = !checks.empty();
	for (auto &check : checks) {
		passed = passed && check.second.empty();
		for (const auto &private_token : private_tokens) if (!private_token.empty()) {
			std::size_t at = 0;
			while ((at = check.second.find(private_token, at)) != std::string::npos) {
				check.second.replace(at, private_token.size(), "[redacted]"); at += 10;
			}
		}
	}
	std::cout << "{\"passed\":" << (passed ? "true" : "false") << ",\"native_used\":true,\"checks\":[";
	for (std::size_t i = 0; i < checks.size(); ++i) {
		if (i) std::cout << ',';
		std::cout << "{\"name\":" << duckonwire::JsonString(checks[i].first) << ",\"passed\":" << (checks[i].second.empty() ? "true" : "false");
		if (!checks[i].second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(checks[i].second);
		std::cout << '}';
	}
	std::cout << "]}\n";
	return passed ? 0 : 1;
}
