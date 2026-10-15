#pragma once

// Bounded TLS/SCRAM and PostgreSQL protocol fixture shared in shape with
// pgwire_protocol_tests.cpp. Error text deliberately excludes SQL and credentials.
#include "native_server.hpp"
#include <openssl/ssl.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>

namespace read_copy_pg_test {
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

} // namespace read_copy_pg_test
