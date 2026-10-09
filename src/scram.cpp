#include "duckonwire/scram.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace duckonwire {
namespace {
constexpr std::size_t KEY_SIZE = 32;
constexpr std::size_t MAX_MESSAGE = 4096;
constexpr std::uint32_t MIN_ITERATIONS = 4096;
constexpr std::uint32_t MAX_ITERATIONS = 1000000;
[[noreturn]] void Invalid() { throw std::invalid_argument("Invalid SCRAM authentication"); }
std::string Encode(const std::string &bytes) {
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    const int count = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&result[0]),
                                     reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    if (count < 0) throw std::runtime_error("OpenSSL base64 encoding failed");
    result.resize(static_cast<std::size_t>(count)); return result;
}
std::string Decode(const std::string &text, std::size_t minimum, std::size_t maximum) {
    if (text.empty() || text.size() % 4 || text.size() > 4 * ((maximum + 2) / 3)) Invalid();
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) Invalid();
        if (c == '=' && i < text.size() - 2) Invalid();
    }
    std::string bytes(3 * (text.size() / 4), '\0');
    int count = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(&bytes[0]),
                                reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
    if (count < 0) Invalid();
    if (text.back() == '=') --count;
    if (text.size() > 1 && text[text.size() - 2] == '=') --count;
    if (count < 0 || static_cast<std::size_t>(count) < minimum || static_cast<std::size_t>(count) > maximum) Invalid();
    bytes.resize(static_cast<std::size_t>(count));
    if (Encode(bytes) != text) Invalid(); // Reject noncanonical padding and trailing bits.
    return bytes;
}
std::string Digest(const std::string &bytes) {
    std::string result(KEY_SIZE, '\0'); unsigned int count = 0;
    if (!EVP_Digest(bytes.data(), bytes.size(), reinterpret_cast<unsigned char *>(&result[0]), &count, EVP_sha256(), nullptr) || count != KEY_SIZE)
        throw std::runtime_error("OpenSSL SHA-256 failed");
    return result;
}
std::string Mac(const std::string &key, const std::string &bytes) {
    std::string result(KEY_SIZE, '\0'); unsigned int count = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
              reinterpret_cast<unsigned char *>(&result[0]), &count) || count != KEY_SIZE)
        throw std::runtime_error("OpenSSL HMAC failed");
    return result;
}
std::string Random(std::size_t size) {
    std::string bytes(size, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&bytes[0]), static_cast<int>(size)) != 1)
        throw std::runtime_error("OpenSSL secure random generation failed");
    return bytes;
}
struct Verifier { std::uint32_t iterations; std::string salt, stored, server; };
Verifier Parse(const std::string &text) {
    const std::string prefix = "SCRAM-SHA-256$";
    if (text.size() > 512 || text.compare(0, prefix.size(), prefix) != 0) Invalid();
    const auto colon = text.find(':', prefix.size());
    const auto dollar = text.find('$', colon == std::string::npos ? text.size() : colon + 1);
    const auto key_colon = text.find(':', dollar == std::string::npos ? text.size() : dollar + 1);
    if (colon == std::string::npos || dollar == std::string::npos || key_colon == std::string::npos || colon == prefix.size()) Invalid();
    std::uint64_t iterations = 0;
    for (auto i = prefix.size(); i < colon; ++i) {
        if (text[i] < '0' || text[i] > '9') Invalid();
        iterations = iterations * 10 + static_cast<unsigned>(text[i] - '0');
        if (iterations > MAX_ITERATIONS) Invalid();
    }
    if (iterations < MIN_ITERATIONS) Invalid();
    return {static_cast<std::uint32_t>(iterations), Decode(text.substr(colon + 1, dollar - colon - 1), 16, 64),
            Decode(text.substr(dollar + 1, key_colon - dollar - 1), KEY_SIZE, KEY_SIZE),
            Decode(text.substr(key_colon + 1), KEY_SIZE, KEY_SIZE)};
}
std::map<char, std::string> Attributes(const std::string &text) {
    if (text.empty() || text.size() > MAX_MESSAGE) Invalid();
    std::map<char, std::string> result;
    std::size_t begin = 0;
    for (;;) {
        auto end = text.find(',', begin); if (end == std::string::npos) end = text.size();
        if (end - begin < 2 || text[begin + 1] != '=' || !result.emplace(text[begin], text.substr(begin + 2, end - begin - 2)).second) Invalid();
        for (auto i = begin; i < end; ++i) if (static_cast<unsigned char>(text[i]) < 0x20 || static_cast<unsigned char>(text[i]) > 0x7e) Invalid();
        if (end == text.size()) break;
        begin = end + 1; if (begin == text.size()) Invalid();
    }
    return result;
}
}
std::string MakeScramVerifier(const std::string &password, std::uint32_t iterations) {
    if (password.size() < 32 || password.size() > 1024 ||
        std::any_of(password.begin(), password.end(), [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
        throw std::invalid_argument("SCRAM passwords require 32..1024 printable ASCII bytes");
    if (iterations < MIN_ITERATIONS || iterations > MAX_ITERATIONS) throw std::invalid_argument("SCRAM iteration count must be 4096..1000000");
    const auto salt = Random(24);
    std::string salted(KEY_SIZE, '\0');
    if (!PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.data()),
                          static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(), KEY_SIZE,
                          reinterpret_cast<unsigned char *>(&salted[0]))) throw std::runtime_error("OpenSSL PBKDF2 failed");
    auto client = Mac(salted, "Client Key");
    const auto stored = Digest(client); const auto server = Mac(salted, "Server Key");
    OPENSSL_cleanse(&salted[0], salted.size()); OPENSSL_cleanse(&client[0], client.size());
    return "SCRAM-SHA-256$" + std::to_string(iterations) + ":" + Encode(salt) + "$" + Encode(stored) + ":" + Encode(server);
}
void ValidateScramVerifier(const std::string &verifier) { (void) Parse(verifier); }
std::string MockScramVerifier(const std::string &server_verifier, const std::string &identity) {
    const auto parsed = Parse(server_verifier);
    const auto salt = Mac(parsed.server, "DuckOnWire mock salt:" + identity).substr(0, parsed.salt.size());
    const auto stored = Mac(parsed.server, "DuckOnWire mock stored key:" + identity);
    const auto server = Mac(parsed.server, "DuckOnWire mock server key:" + identity);
    return "SCRAM-SHA-256$" + std::to_string(parsed.iterations) + ":" + Encode(salt) + "$" + Encode(stored) + ":" + Encode(server);
}
ScramExchange::ScramExchange(const std::string &verifier) {
    auto parsed = Parse(verifier); iterations_ = parsed.iterations; salt_ = std::move(parsed.salt);
    stored_key_ = std::move(parsed.stored); server_key_ = std::move(parsed.server);
}
std::string ScramExchange::Challenge(const std::string &client_first) {
    if (challenged_ || client_first.size() > MAX_MESSAGE || client_first.size() < 3) Invalid();
    gs2_ = client_first.substr(0, 3);
    if (gs2_ != "n,," && gs2_ != "y,,") Invalid(); // SCRAM-PLUS and authorization identities are unavailable.
    first_ = client_first.substr(3); const auto attrs = Attributes(first_);
    if (attrs.size() != 2 || !attrs.count('n') || !attrs.count('r') || first_.compare(0, 2, "n=") || first_.find(",r=") == std::string::npos) Invalid();
    const auto &username = attrs.at('n');
    for (std::size_t i = 0; i < username.size(); ++i) if (username[i] == '=') {
        if (username.compare(i, 3, "=2C") && username.compare(i, 3, "=3D")) Invalid();
        i += 2;
    }
    const auto &client_nonce = attrs.at('r');
    if (client_nonce.size() < 16 || client_nonce.size() > 256 || client_nonce.find(' ') != std::string::npos) Invalid();
    nonce_ = client_nonce + Encode(Random(32));
    challenge_ = "r=" + nonce_ + ",s=" + Encode(salt_) + ",i=" + std::to_string(iterations_);
    challenged_ = true; return challenge_;
}
std::string ScramExchange::Finish(const std::string &client_final) {
    if (!challenged_ || finished_ || client_final.size() > MAX_MESSAGE) Invalid();
    finished_ = true;
    const auto attrs = Attributes(client_final);
    if (attrs.size() != 3 || !attrs.count('c') || !attrs.count('r') || !attrs.count('p') || client_final.compare(0, 2, "c=") ||
        client_final.find(",r=") == std::string::npos || attrs.at('r') != nonce_ ||
        Decode(attrs.at('c'), 3, 3) != gs2_) Invalid();
    const auto proof_at = client_final.rfind(",p=");
    if (proof_at == std::string::npos || client_final.find(',', proof_at + 1) != std::string::npos) Invalid();
    const auto proof = Decode(attrs.at('p'), KEY_SIZE, KEY_SIZE);
    const auto auth = first_ + "," + challenge_ + "," + client_final.substr(0, proof_at);
    const auto signature = Mac(stored_key_, auth);
    std::string client_key(KEY_SIZE, '\0');
    for (std::size_t i = 0; i < KEY_SIZE; ++i) client_key[i] = static_cast<char>(static_cast<unsigned char>(proof[i]) ^ static_cast<unsigned char>(signature[i]));
    const auto stored = Digest(client_key); OPENSSL_cleanse(&client_key[0], client_key.size());
    if (CRYPTO_memcmp(stored.data(), stored_key_.data(), KEY_SIZE)) Invalid();
    return "v=" + Encode(Mac(server_key_, auth));
}
} // namespace duckonwire
