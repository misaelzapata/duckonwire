#pragma once

#include <string>
#include <vector>

namespace duckonwire {

struct AccessRule {
    std::string name;
    std::string verifier;
    bool read_only = true;
};

struct AccessConfig {
    std::string database;
    std::vector<AccessRule> users;
};

// Reads a private regular TSV file without following its final symlink.
// Cryptographic SCRAM verifier validation belongs to the authentication layer.
AccessConfig ReadAccessConfig(const std::string &path);

} // namespace duckonwire
