#pragma once

#include <string>
#include <vector>

namespace duckonwire {

struct AccessRule {
    std::string name;
    std::string verifier;
    bool read_only = true;
    std::string database;
};

struct AccessConfig {
    // The first configured database remains the default for legacy callers.
    std::string database;
    std::vector<AccessRule> users;
    std::vector<std::string> databases;
};

// Reads a private regular TSV file without following its final symlink.
// Each database record starts a section; following users are scoped to it.
// Cryptographic SCRAM verifier validation belongs to the authentication layer.
AccessConfig ReadAccessConfig(const std::string &path);

} // namespace duckonwire
