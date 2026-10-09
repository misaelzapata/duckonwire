#pragma once

#include <cstdint>
#include <string>

namespace duckonwire {

// Passwords are restricted to 32..1024 printable ASCII bytes. This avoids an
// ambiguous SASLprep profile while retaining randomly generated passwords.
std::string MakeScramVerifier(const std::string &password, std::uint32_t iterations = 65536);
void ValidateScramVerifier(const std::string &verifier);
// Unknown identities receive distinct, deterministic mock challenges per server.
std::string MockScramVerifier(const std::string &server_verifier, const std::string &identity);

// One server-side SCRAM-SHA-256 exchange. No credentials or proofs are logged.
class ScramExchange {
public:
    explicit ScramExchange(const std::string &verifier);
    std::string Challenge(const std::string &client_first);
    // Throws std::invalid_argument for malformed or invalid authentication.
    std::string Finish(const std::string &client_final);
private:
    std::uint32_t iterations_ = 0;
    std::string salt_, stored_key_, server_key_, gs2_, first_, challenge_, nonce_;
    bool challenged_ = false, finished_ = false;
};

} // namespace duckonwire
