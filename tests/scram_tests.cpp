#include "duckonwire/scram.hpp"
#include "duckonwire/json.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
// Public RFC 7677 example, not a live credential. Keys were also independently
// derived with Python hashlib/PBKDF2 before adding this native regression.
const std::string PASSWORD = "pencil";
const std::string SALT = "W22ZaJ0SNY7soEsUEjb6gQ==";
const std::string STORED = "WG5d8oPm3OtcPnkdi4Uo7BkeZkBFzpcXkuLmtbsT4qY=";
const std::string SERVER = "wfPLwcE6nTWhTAmQ7tl2KeoiWGPlZqQxSrmfPwDl2dU=";
const std::string VERIFIER = "SCRAM-SHA-256$4096:" + SALT + "$" + STORED + ":" + SERVER;
const std::string BARE = "n=user,r=rOprNGfwEbeRWgbNEkqO";

void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::string Encode(const std::string &bytes) {
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()), reinterpret_cast<const unsigned char *>(bytes.data()),
                    static_cast<int>(bytes.size())); return result;
}
std::string Decode(const std::string &text) {
    std::string bytes(3 * (text.size() / 4), '\0');
    int size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
                              reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
    Require(size >= 0, "Independent reference decoding failed");
    if (!text.empty() && text.back() == '=') --size;
    if (text.size() > 1 && text[text.size() - 2] == '=') --size;
    bytes.resize(static_cast<std::size_t>(size)); return bytes;
}
std::string Hmac(const std::string &key, const std::string &text) {
    std::string result(SHA256_DIGEST_LENGTH, '\0'); unsigned int size = 0;
    Require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(text.data()),
                 text.size(), reinterpret_cast<unsigned char *>(result.data()), &size) != nullptr && size == SHA256_DIGEST_LENGTH,
            "Independent reference HMAC failed"); return result;
}
std::string Hash(const std::string &text) {
    std::string result(SHA256_DIGEST_LENGTH, '\0');
    Require(SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(),
                   reinterpret_cast<unsigned char *>(result.data())) != nullptr, "Independent reference digest failed"); return result;
}
std::map<char, std::string> Attributes(const std::string &message) {
    std::map<char, std::string> result;
    std::size_t begin = 0;
    for (;;) {
        auto end = message.find(',', begin); if (end == std::string::npos) end = message.size();
        Require(end > begin + 1 && message[begin + 1] == '=', "Independent reference attribute format failed");
        result.emplace(message[begin], message.substr(begin + 2, end - begin - 2));
        if (end == message.size()) return result;
        begin = end + 1;
    }
}
struct ClientProof { std::string final, server_signature, stored, server; };
ClientProof Proof(const std::string &password, const std::string &bare, const std::string &challenge,
                  const std::string &channel = "biws") {
    auto attrs = Attributes(challenge); auto salt = Decode(attrs.at('s'));
    std::string salted(SHA256_DIGEST_LENGTH, '\0');
    Require(PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                            reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
                            std::stoi(attrs.at('i')), EVP_sha256(), SHA256_DIGEST_LENGTH,
                            reinterpret_cast<unsigned char *>(salted.data())) == 1, "Independent reference PBKDF2 failed");
    auto client = Hmac(salted, "Client Key"); auto stored = Hash(client); auto server = Hmac(salted, "Server Key");
    const auto without_proof = "c=" + channel + ",r=" + attrs.at('r');
    const auto auth = bare + "," + challenge + "," + without_proof;
    auto signature = Hmac(stored, auth); auto proof = client;
    for (std::size_t i = 0; i < proof.size(); ++i) proof[i] ^= signature[i];
    return {without_proof + ",p=" + Encode(proof), "v=" + Encode(Hmac(server, auth)), Encode(stored), Encode(server)};
}
void Rejected(const std::function<void()> &call) {
    try { call(); } catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Invalid SCRAM input was accepted");
}
void BadFinal(const std::function<std::string(const std::string &, const ClientProof &)> &mutate) {
    duckonwire::ScramExchange exchange(VERIFIER);
    const auto challenge = exchange.Challenge("n,," + BARE); auto proof = Proof(PASSWORD, BARE, challenge);
    Rejected([&] { exchange.Finish(mutate(challenge, proof)); });
}
} // namespace

int main() {
    std::vector<std::pair<std::string, std::string>> checks;
    auto test = [&](const std::string &name, const std::function<void()> &body) {
        try { body(); checks.emplace_back(name, ""); }
        catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
    };
    test("independent_reference_matches_rfc7677_known_proof_and_signature", [] {
        // https://www.rfc-editor.org/rfc/rfc7677.html#section-3
        const auto challenge = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=" + SALT + ",i=4096";
        auto proof = Proof(PASSWORD, BARE, challenge);
        Require(proof.stored == STORED && proof.server == SERVER, "RFC verifier keys did not match");
        Require(proof.final.substr(proof.final.find(",p=") + 3) == "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=", "RFC client proof did not match");
        Require(proof.server_signature == "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=", "RFC server signature did not match");
    });
    test("native_exchange_matches_independent_client_and_server_signature", [] {
        duckonwire::ValidateScramVerifier(VERIFIER); duckonwire::ScramExchange exchange(VERIFIER);
        const auto challenge = exchange.Challenge("n,," + BARE); const auto attrs = Attributes(challenge);
        Require(attrs.at('r').find("rOprNGfwEbeRWgbNEkqO") == 0 && attrs.at('r').size() > 20 && attrs.at('s') == SALT && attrs.at('i') == "4096", "Native challenge fields differ");
        const auto proof = Proof(PASSWORD, BARE, challenge);
        Require(exchange.Finish(proof.final) == proof.server_signature, "Native server signature differs");
    });
    test("generated_verifier_uses_independent_password_derivation_and_random_salt", [] {
        const std::string password = "TEST-ONLY-ASCII-password-32-characters";
        const auto a = duckonwire::MakeScramVerifier(password, 4096), b = duckonwire::MakeScramVerifier(password, 4096);
        Require(a != b, "Verifier reused a salt"); duckonwire::ValidateScramVerifier(a);
        duckonwire::ScramExchange exchange(a); auto challenge = exchange.Challenge("n,," + BARE); auto proof = Proof(password, BARE, challenge);
        Require(exchange.Finish(proof.final) == proof.server_signature, "Generated verifier mismatched independent client");
    });
    test("random_server_nonce_prevents_cross_exchange_replay", [] {
        duckonwire::ScramExchange first(VERIFIER), second(VERIFIER);
        const auto a = first.Challenge("n,," + BARE), b = second.Challenge("n,," + BARE);
        Require(Attributes(a).at('r') != Attributes(b).at('r'), "Server nonce repeated");
        const auto proof = Proof(PASSWORD, BARE, a); Rejected([&] { second.Finish(proof.final); });
    });
    test("exchange_cannot_repeat_challenge_finish_or_recover_after_bad_proof", [] {
        duckonwire::ScramExchange exchange(VERIFIER); const auto challenge = exchange.Challenge("n,," + BARE);
        Rejected([&] { exchange.Challenge("n,," + BARE); }); auto proof = Proof(PASSWORD, BARE, challenge);
        Require(exchange.Finish(proof.final) == proof.server_signature, "Initial valid proof failed"); Rejected([&] { exchange.Finish(proof.final); });
        duckonwire::ScramExchange failed(VERIFIER); auto bad = Proof("wrong-password", BARE, failed.Challenge("n,," + BARE));
        Rejected([&] { failed.Finish(bad.final); }); Rejected([&] { failed.Finish(proof.final); });
        duckonwire::ScramExchange premature(VERIFIER); Rejected([&] { premature.Finish(proof.final); });
    });
    test("incorrect_password_and_corrupted_proof_rejected", [] {
        BadFinal([](const std::string &challenge, const ClientProof &) { return Proof("wrong-password", BARE, challenge).final; });
        BadFinal([](const std::string &, const ClientProof &proof) { auto final = proof.final; final.back() = 'A'; return final; });
    });
    test("wrong_nonce_and_channel_binding_rejected_even_with_valid_math", [] {
        BadFinal([](const std::string &, const ClientProof &proof) { auto final = proof.final; final.insert(final.find(",p="), "replayed"); return final; });
        BadFinal([](const std::string &challenge, const ClientProof &) { return Proof(PASSWORD, BARE, challenge, "eSws").final; });
        duckonwire::ScramExchange capable(VERIFIER); auto challenge = capable.Challenge("y,," + BARE); auto proof = Proof(PASSWORD, BARE, challenge, "eSws");
        Require(capable.Finish(proof.final) == proof.server_signature, "Valid non-PLUS y header failed");
    });
    test("duplicate_missing_extended_and_control_attributes_rejected", [] {
        for (const auto &first : {"n,,n=user,r=abcdefghijklmnop,r=duplicate", "n,,n=user", "n,,n=user,r=abcdefghijklmnop,m=required", "n,,r=abcdefghijklmnop,n=user", "n,,n=user,r=abcdefghijklmnop,", "n,,n=user,r=abcdefghijklmno\n"}) {
            duckonwire::ScramExchange exchange(VERIFIER); Rejected([&] { exchange.Challenge(first); });
        }
        BadFinal([](const std::string &, const ClientProof &proof) { return proof.final + ",p=duplicate"; });
        BadFinal([](const std::string &, const ClientProof &proof) { return proof.final + ",m=unsupported"; });
        BadFinal([](const std::string &, const ClientProof &proof) { auto final = proof.final; final.erase(0, final.find(',') + 1); return final; });
    });
    test("plus_authzid_and_invalid_username_escape_rejected", [] {
        for (const auto &first : {"p=tls-server-end-point,,n=user,r=abcdefghijklmnop", "n,a=admin,n=user,r=abcdefghijklmnop", "n,,n=bad=XXname,r=abcdefghijklmnop"}) {
            duckonwire::ScramExchange exchange(VERIFIER); Rejected([&] { exchange.Challenge(first); });
        }
    });
    test("nonce_and_message_bounds_enforced", [] {
        for (const auto &nonce : {std::string(15, 'a'), std::string(257, 'a'), std::string(16, ' ')}) {
            duckonwire::ScramExchange exchange(VERIFIER); Rejected([&] { exchange.Challenge("n,,n=user,r=" + nonce); });
        }
        duckonwire::ScramExchange exchange(VERIFIER); Rejected([&] { exchange.Challenge(std::string(4097, 'a')); });
        exchange.Challenge("n,," + BARE); Rejected([&] { exchange.Finish(std::string(4097, 'a')); });
    });
    test("verifier_iteration_salt_key_and_canonical_base64_bounds", [] {
        for (const auto &iterations : {"0", "4095", "1000001", "-4096", "4096x", "184467440737095516160000"})
            Rejected([&] { duckonwire::ValidateScramVerifier("SCRAM-SHA-256$" + std::string(iterations) + ":" + SALT + "$" + STORED + ":" + SERVER); });
        for (const auto &salt : std::vector<std::string>{Encode(std::string(15, 'a')), Encode(std::string(65, 'a')), "W22ZaJ0SNY7soEsUEjb6gR=="})
            Rejected([&] { duckonwire::ValidateScramVerifier("SCRAM-SHA-256$4096:" + salt + "$" + STORED + ":" + SERVER); });
        for (const auto &key : std::vector<std::string>{Encode(std::string(31, 'a')), Encode(std::string(33, 'a')), "invalid-base64"}) {
            Rejected([&] { duckonwire::ValidateScramVerifier("SCRAM-SHA-256$4096:" + SALT + "$" + key + ":" + SERVER); });
            Rejected([&] { duckonwire::ValidateScramVerifier("SCRAM-SHA-256$4096:" + SALT + "$" + STORED + ":" + key); });
        }
        duckonwire::ValidateScramVerifier("SCRAM-SHA-256$1000000:" + Encode(std::string(64, 'a')) + "$" + STORED + ":" + SERVER);
        Rejected([] { duckonwire::ValidateScramVerifier("SCRAM-SHA-256$4096:missing"); });
        Rejected([] { duckonwire::ValidateScramVerifier(std::string(513, 'a')); });
    });
    test("password_generator_ascii_length_and_iteration_bounds", [] {
        for (const auto &password : {std::string(31, 'a'), std::string(1025, 'a'), std::string(32, '\n'), std::string(32, static_cast<char>(0xff))})
            Rejected([&] { duckonwire::MakeScramVerifier(password, 4096); });
        for (unsigned iterations : {4095u, 1000001u}) Rejected([&] { duckonwire::MakeScramVerifier(std::string(32, 'a'), iterations); });
        duckonwire::ValidateScramVerifier(duckonwire::MakeScramVerifier(std::string(1024, 'a'), 4096));
    });
    bool passed = !checks.empty(); std::cout << "{\"checks\":[";
    for (std::size_t i = 0; i < checks.size(); ++i) {
        if (i) std::cout << ','; const auto &check = checks[i]; passed = passed && check.second.empty();
        std::cout << "{\"name\":" << duckonwire::JsonString(check.first) << ",\"passed\":" << (check.second.empty() ? "true" : "false");
        if (!check.second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(check.second);
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n"; return passed ? 0 : 1;
}
