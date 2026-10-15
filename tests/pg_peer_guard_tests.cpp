#include "duckonwire/json.hpp"
#include "quack_security_test.hpp"

#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <functional>
#include <iostream>
#include <map>
#include <optional>

namespace {
using Bytes = std::vector<unsigned char>;
using test_support::Require;
using quack_security_test::Fixture;
namespace fs = std::filesystem;

void U16(Bytes &out, std::uint16_t value) {
    out.push_back(static_cast<unsigned char>(value >> 8)); out.push_back(static_cast<unsigned char>(value));
}
void U32(Bytes &out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>(value >> shift));
}
std::uint32_t Read32(const Bytes &bytes, std::size_t offset = 0) {
    Require(offset + 4 <= bytes.size(), "Truncated fixture backend integer");
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) result = (result << 8) | bytes[offset + index];
    return result;
}
void String(Bytes &out, const std::string &value) { out.insert(out.end(), value.begin(), value.end()); out.push_back(0); }
struct Reply { char type; Bytes body; };
std::string ErrorCode(const Reply &reply) {
    if (reply.type != 'E') return {};
    for (std::size_t offset = 0; offset < reply.body.size() && reply.body[offset];) {
        const auto field = reply.body[offset++];
        const auto end = std::find(reply.body.begin() + offset, reply.body.end(), 0);
        Require(end != reply.body.end(), "Unterminated fixture backend error");
        const std::string value(reply.body.begin() + offset, end);
        offset = static_cast<std::size_t>(end - reply.body.begin()) + 1;
        if (field == 'C') return value;
    }
    return {};
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
std::string Hmac(const std::string &key, const std::string &message) {
    std::string output(32, '\0'); unsigned size = 0;
    Require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char *>(message.data()), message.size(),
        reinterpret_cast<unsigned char *>(output.data()), &size) && size == 32, "Fixture HMAC failed");
    return output;
}
std::string Digest(const std::string &message) {
    std::string output(32, '\0'); unsigned size = 0;
    Require(EVP_Digest(message.data(), message.size(), reinterpret_cast<unsigned char *>(output.data()),
        &size, EVP_sha256(), nullptr) && size == 32, "Fixture SHA256 failed");
    return output;
}
std::pair<std::string, std::string> ScramProof(const std::string &password, const std::string &bare,
        const std::string &challenge) {
    std::map<char, std::string> attributes;
    for (std::size_t offset = 0; offset < challenge.size();) {
        auto end = challenge.find(',', offset); if (end == std::string::npos) end = challenge.size();
        Require(end > offset + 2 && challenge[offset + 1] == '=', "Invalid fixture SCRAM challenge");
        attributes.emplace(challenge[offset], challenge.substr(offset + 2, end - offset - 2)); offset = end + 1;
    }
    const auto salt = Unbase64(attributes.at('s')); std::string salted(32, '\0');
    Require(PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
        std::stoi(attributes.at('i')), EVP_sha256(), 32, reinterpret_cast<unsigned char *>(salted.data())) == 1,
        "Fixture PBKDF2 failed");
    auto client = Hmac(salted, "Client Key"); const auto final = "c=biws,r=" + attributes.at('r');
    const auto authentication = bare + ',' + challenge + ',' + final;
    const auto signature = Hmac(Digest(client), authentication);
    for (std::size_t index = 0; index < client.size(); ++index) client[index] ^= signature[index];
    return {final + ",p=" + Base64(client), "v=" + Base64(Hmac(Hmac(salted, "Server Key"), authentication))};
}

class Wire {
public:
    explicit Wire(int port, const std::string &source = "127.0.0.1") {
        socket_ = socket(AF_INET, SOCK_STREAM, 0); Require(socket_ >= 0, "Cannot create fixture PG socket");
        try {
            timeval timeout {5, 0};
            Require(!setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) &&
                !setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)), "Cannot bound fixture PG socket");
#ifdef SO_NOSIGPIPE
            int no_signal = 1; setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
            sockaddr_in address {}; address.sin_family = AF_INET;
            Require(inet_pton(AF_INET, source.c_str(), &address.sin_addr) == 1 &&
                !bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "Cannot bind fixture PG source peer");
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<std::uint16_t>(port));
            Require(!connect(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "Cannot connect fixture PG socket");
        } catch (...) { Close(); throw; }
    }
    ~Wire() { Close(); }
    Wire(const Wire &) = delete;
    Wire &operator=(const Wire &) = delete;
    void Close() {
        if (tls_) { SSL_free(tls_); tls_ = nullptr; }
        if (context_) { SSL_CTX_free(context_); context_ = nullptr; }
        if (socket_ >= 0) { shutdown(socket_, SHUT_RDWR); close(socket_); socket_ = -1; }
    }
    void Send(const Bytes &bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            int count;
            if (tls_) count = SSL_write(tls_, bytes.data() + offset, static_cast<int>(bytes.size() - offset));
            else {
#ifdef MSG_NOSIGNAL
                count = static_cast<int>(send(socket_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL));
#else
                count = static_cast<int>(send(socket_, bytes.data() + offset, bytes.size() - offset, 0));
#endif
            }
            if (count < 0 && errno == EINTR) continue;
            Require(count > 0, "Fixture PG send failed"); offset += static_cast<std::size_t>(count);
        }
    }
    void Message(char type, const Bytes &body = {}) {
        Bytes frame {static_cast<unsigned char>(type)}; U32(frame, static_cast<std::uint32_t>(body.size() + 4));
        frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
    }
    bool Receive(void *output, std::size_t size) {
        auto bytes = static_cast<unsigned char *>(output); std::size_t offset = 0;
        while (offset < size) {
            errno = 0;
            const int count = tls_ ? SSL_read(tls_, bytes + offset, static_cast<int>(size - offset)) :
                static_cast<int>(recv(socket_, bytes + offset, size - offset, 0));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 && !offset && errno != EAGAIN && errno != EWOULDBLOCK) return false;
            Require(count > 0, "Fixture PG receive timed out or frame truncated"); offset += static_cast<std::size_t>(count);
        }
        return true;
    }
    std::optional<Reply> Next() {
        unsigned char type; if (!Receive(&type, 1)) return {};
        Bytes header(4); Require(Receive(header.data(), 4), "Missing fixture backend header");
        const auto length = Read32(header); Require(length >= 4 && length <= 16 * 1024 * 1024, "Invalid fixture backend frame");
        Reply reply {static_cast<char>(type), Bytes(length - 4)};
        if (!reply.body.empty()) Require(Receive(reply.body.data(), reply.body.size()), "Missing fixture backend body");
        return reply;
    }
    bool TLSAccepted() {
        Bytes request; U32(request, 8); U32(request, 80877103); Send(request);
        unsigned char answer = 0;
        return Receive(&answer, 1) && answer == 'S';
    }
    void Login(const Fixture &fixture, const std::string &user = "reader", bool wrong_password = false) {
        Require(TLSAccepted(), "Protected PG TLS negotiation was rejected");
        const auto certificate = fixture.root.path() / "ca.pem";
        context_ = SSL_CTX_new(TLS_client_method());
        Require(context_ && SSL_CTX_load_verify_locations(context_, certificate.c_str(), nullptr) == 1 &&
            SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) == 1, "Cannot configure verified fixture TLS");
        SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr); tls_ = SSL_new(context_);
        Require(tls_ && SSL_set_fd(tls_, socket_) == 1 && SSL_set_tlsext_host_name(tls_, "localhost") == 1 &&
            SSL_set1_host(tls_, "localhost") == 1 && SSL_connect(tls_) == 1 && SSL_get_verify_result(tls_) == X509_V_OK,
            "Fixture PG certificate or hostname verification failed");
        Bytes body; U32(body, 196608); String(body, "user"); String(body, user);
        String(body, "database"); String(body, "duckonwire"); body.push_back(0);
        Bytes frame; U32(frame, static_cast<std::uint32_t>(body.size() + 4)); frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
        auto response = Next(); Require(response && response->type == 'R' && Read32(response->body) == 10, "Missing fixture SCRAM mechanism");
        std::string random(18, '\0'); Require(RAND_bytes(reinterpret_cast<unsigned char *>(random.data()), 18) == 1, "Cannot create fixture SCRAM nonce");
        const auto bare = "n=,r=" + Base64(random), first = "n,," + bare;
        body.clear(); String(body, "SCRAM-SHA-256"); U32(body, static_cast<std::uint32_t>(first.size()));
        body.insert(body.end(), first.begin(), first.end()); Message('p', body);
        response = Next(); Require(response && response->type == 'R' && Read32(response->body) == 11, "Missing fixture SCRAM continuation");
        const auto password = wrong_password ? fixture.Password() + "-incorrect" : fixture.Password();
        const auto proof = ScramProof(password, bare, std::string(response->body.begin() + 4, response->body.end()));
        Message('p', Bytes(proof.first.begin(), proof.first.end())); response = Next();
        if (wrong_password) {
            Require(response && ErrorCode(*response) == "28P01", "Incorrect fixture credential did not fail SCRAM");
            Require(!Next(), "Failed SCRAM left a usable fixture session"); return;
        }
        Require(response && response->type == 'R' && Read32(response->body) == 12 &&
            std::string(response->body.begin() + 4, response->body.end()) == proof.second, "Fixture SCRAM server proof mismatch");
        Ready();
    }
    void Ready() {
        for (unsigned message = 0; message < 128; ++message) {
            const auto reply = Next(); Require(bool(reply) && reply->type != 'E', "Fixture PG did not reach ReadyForQuery");
            if (reply->type == 'Z') return;
        }
        throw std::runtime_error("Fixture PG reply bound exceeded");
    }
    void Query(const std::string &sql = "SELECT 42") { Bytes body; String(body, sql); Message('Q', body); Ready(); }
    void BannedQuery() {
        Bytes body; String(body, "SELECT 42"); Message('Q', body);
        const auto response = Next(); Require(response && ErrorCode(*response) == "53300", "Banned peer query did not report SQLSTATE53300");
        Require(!Next(), "Banned peer session was not disconnected");
    }
private:
    int socket_ = -1;
    SSL_CTX *context_ = nullptr;
    SSL *tls_ = nullptr;
};

// Probe an owned loopback listener so this never connects to another machine.
// Linux permits 127.0.0.2 directly; macOS can use another already assigned IPv4
// address as the source of the local connection without changing interfaces.
std::string OtherLocalPeer() {
    std::vector<std::string> candidates {"127.0.0.2"};
    ifaddrs *raw = nullptr; Require(getifaddrs(&raw) == 0, "Cannot inspect fixture source interfaces");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces(raw, freeifaddrs);
    for (auto entry = raw; entry; entry = entry->ifa_next) if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET &&
        (entry->ifa_flags & IFF_UP) && !(entry->ifa_flags & IFF_LOOPBACK)) {
        char address[INET_ADDRSTRLEN] {};
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(entry->ifa_addr)->sin_addr, address, sizeof(address))) candidates.emplace_back(address);
    }
    const int listener = socket(AF_INET, SOCK_STREAM, 0); Require(listener >= 0, "Cannot create fixture source probe");
    sockaddr_in address {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), size) || listen(listener, 1) ||
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size)) {
        close(listener); throw std::runtime_error("Cannot bind fixture source probe");
    }
    for (const auto &candidate : candidates) {
        try {
            Wire source(ntohs(address.sin_port), candidate);
            sockaddr_in peer {}; size = sizeof(peer);
            const auto accepted = accept(listener, reinterpret_cast<sockaddr *>(&peer), &size);
            Require(accepted >= 0, "Cannot accept fixture source probe"); close(accepted);
            if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { close(listener); return candidate; }
        } catch (const std::exception &) {}
    }
    close(listener); throw std::runtime_error("Host has no second usable local IPv4 source peer");
}
void FailAuthentication(const Fixture &fixture, const std::string &user = "reader") {
    Wire attempt(fixture.server->pg_port()); attempt.Login(fixture, user, true);
}
} // namespace

int main(int argc, char **argv) {
    fs::path server, parent = fs::temp_directory_path();
    for (int argument = 1; argument < argc; ++argument) {
        const std::string option(argv[argument]);
        if (option == "--server" && argument + 1 < argc) server = argv[++argument];
        else if (option == "--fixture-parent" && argument + 1 < argc) parent = argv[++argument];
        else { std::cerr << "Usage: pg-peer-guard-tests --server BINARY [--fixture-parent DIRECTORY]\n"; return 2; }
    }
    if (server.empty()) { std::cerr << "Missing --server\n"; return 2; }
    std::vector<std::pair<std::string, std::string>> checks;
    const auto test = [&](const std::string &name, const std::function<void()> &body) {
        try { body(); checks.emplace_back(name, ""); }
        catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
    };
    test("pg_scram_failures_block_new_and_existing_sessions_until_ban_expiry", [&] {
        Fixture fixture(parent, server, {"--auth-max-failures", "2"});
        Wire existing(fixture.server->pg_port()); existing.Login(fixture); existing.Query();
        FailAuthentication(fixture); FailAuthentication(fixture, "unknown_account");
        existing.BannedQuery();
        { Wire blocked(fixture.server->pg_port()); Require(!blocked.TLSAccepted(), "Banned peer reached TLS negotiation"); }
        quack_security_test::WaitForBanExpiry();
        Wire recovered(fixture.server->pg_port()); recovered.Login(fixture); recovered.Query();
        fixture.Stop();
    });
    test("pg_authentication_ban_denies_native_new_and_reused_sessions", [&] {
        Fixture fixture(parent, server, {"--auth-max-failures", "2"}); auto existing = fixture.Login();
        Require(quack_security_test::Scalar(*existing->connection, "SELECT 42") == 42,
            "Native baseline session was not usable before PG peer ban");
        FailAuthentication(fixture); FailAuthentication(fixture);
        bool denied = false;
        try {
            const auto result = existing->connection->Query("SELECT 42");
            denied = result && result->HasError();
        } catch (const std::exception &error) {
            denied = std::string(error.what()).find("Quack request rejected") != std::string::npos;
        }
        Require(denied, "Native existing session escaped PG peer ban");
        Require(quack_security_test::RejectCredential(fixture.Endpoint(), fixture.Credential("reader")), "Native login escaped PG peer ban");
        quack_security_test::WaitForBanExpiry(); auto recovered = fixture.Login();
        Require(quack_security_test::Scalar(*recovered->connection, "SELECT 42") == 42, "Native peer did not recover after PG ban expiry");
        fixture.Stop();
    });
    test("pg_ban_uses_actual_socket_peer_without_harming_other_local_ip", [&] {
        const auto source = OtherLocalPeer(); Fixture fixture(parent, server, {"--auth-max-failures", "2"});
        Wire other(fixture.server->pg_port(), source); other.Login(fixture); other.Query();
        FailAuthentication(fixture); FailAuthentication(fixture); other.Query();
        Wire fresh_other(fixture.server->pg_port(), source); fresh_other.Login(fixture); fresh_other.Query();
        fixture.Stop();
    });
    test("pg_request_overuse_disconnects_authenticated_session", [&] {
        Fixture fixture(parent, server, {"--max-requests-per-minute", "3"});
        Wire client(fixture.server->pg_port()); client.Login(fixture);
        client.Query(); client.Query(); client.Query(); client.BannedQuery(); fixture.Stop();
    });
    test("pg_parse_budget_blocks_before_receiving_oversized_body", [&] {
        Fixture fixture(parent, server, {"--max-requests-per-minute", "2"});
        Wire client(fixture.server->pg_port()); client.Login(fixture);
        Bytes parse; String(parse, "statement"); String(parse, "SELECT 42"); U16(parse, 0);
        client.Message('P', parse);
        const auto parsed = client.Next(); Require(parsed && parsed->type == '1', "Fixture parse did not complete");
        client.Message('H');
        Bytes oversized {static_cast<unsigned char>('P')}; U32(oversized, 16 * 1024 * 1024); client.Send(oversized);
        const auto denied = client.Next(); Require(denied && ErrorCode(*denied) == "53300", "Parser abuse was not rejected before body receive");
        Require(!client.Next(), "Over-budget parser socket remained usable"); fixture.Stop();
    });
    test("pg_connection_overuse_blocks_tls_and_existing_sessions_then_recovers", [&] {
        Fixture fixture(parent, server, {"--max-connections-per-minute", "3"});
        Wire existing(fixture.server->pg_port()); existing.Login(fixture);
        for (unsigned connection = 0; connection < 2; ++connection) {
            Wire admitted(fixture.server->pg_port()); Require(admitted.TLSAccepted(), "Connection denied within configured rate");
        }
        { Wire excess(fixture.server->pg_port()); Require(!excess.TLSAccepted(), "Excess connection reached TLS negotiation"); }
        existing.BannedQuery(); quack_security_test::WaitForBanExpiry();
        Wire recovered(fixture.server->pg_port()); recovered.Login(fixture); recovered.Query(); fixture.Stop();
    });
    test("pg_normal_disconnects_and_tls_rejections_do_not_count_as_credentials", [&] {
        Fixture fixture(parent, server, {"--auth-max-failures", "1"});
        for (unsigned connection = 0; connection < 3; ++connection) {
            Wire disconnected(fixture.server->pg_port()); Require(disconnected.TLSAccepted(), "Normal disconnect exhausted authentication budget");
        }
        {
            Wire plaintext(fixture.server->pg_port()); Bytes body; U32(body, 196608); body.push_back(0);
            Bytes startup; U32(startup, static_cast<std::uint32_t>(body.size() + 4)); startup.insert(startup.end(), body.begin(), body.end());
            plaintext.Send(startup); const auto denied = plaintext.Next();
            Require(denied && ErrorCode(*denied) == "28000", "Unencrypted startup did not require TLS");
        }
        Wire valid(fixture.server->pg_port()); valid.Login(fixture); valid.Query(); fixture.Stop();
    });
    bool passed = !checks.empty(); std::cout << "{\"checks\":[";
    for (std::size_t index = 0; index < checks.size(); ++index) {
        if (index) std::cout << ',';
        const auto &check = checks[index]; passed = passed && check.second.empty();
        std::cout << "{\"name\":" << duckonwire::JsonString(check.first) << ",\"passed\":" << (check.second.empty() ? "true" : "false");
        if (!check.second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(check.second);
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
