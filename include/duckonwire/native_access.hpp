#pragma once

#include "duckonwire/access.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace duckdb { class DuckDB; }
namespace duckonwire {
class PeerGuard;

// Installs account policy on one native engine before its Quack listener starts.
// Each engine receives only grants for its published database name.
void ConfigureNativeAccess(duckdb::DuckDB &database, const std::string &name,
                           const AccessConfig &access, std::shared_ptr<PeerGuard> guard,
                           bool force_read_only = false,
                           std::shared_ptr<std::atomic<bool>> available = {});
// Reuse the startup stored-definition audit before publishing every version.
void AuditNativeStoredDefinitions(duckdb::DuckDB &database);
// Local copies retain shared-token login, with the same immutable SQL boundary.
void ConfigureReadCopyNativeAccess(duckdb::DuckDB &database, const std::string &token,
                                   std::shared_ptr<std::atomic<bool>> available);
}
