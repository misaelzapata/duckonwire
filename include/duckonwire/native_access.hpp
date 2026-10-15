#pragma once

#include "duckonwire/access.hpp"
#include <memory>
#include <string>

namespace duckdb { class DuckDB; }
namespace duckonwire {
class PeerGuard;

// Installs account policy on one native engine before its Quack listener starts.
// Each engine receives only grants for its published database name.
void ConfigureNativeAccess(duckdb::DuckDB &database, const std::string &name,
                           const AccessConfig &access, std::shared_ptr<PeerGuard> guard);
}
