#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace duckdb { class DuckDB; }

namespace duckonwire {

struct PgUser {
    std::string name, scram_verifier;
    bool read_only = true;
    std::string database;
};
// Each published database has its own native instance. Instances must outlive
// the server and all sessions; routing never attaches another file to a session.
struct PgDatabase {
    std::string name;
    duckdb::DuckDB *database = nullptr;
};
// Protected mode requires an engine with external access disabled, configuration
// locked, unsigned extensions disabled, and no filesystem/configuration exceptions
// beyond the engine's own storage and spill files. TLS is
// mandatory; authenticated sessions have a 300-second idle/frame receive bound.
struct PgWireOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9496;
    std::string database_name = "duckonwire", tls_cert, tls_key;
    std::vector<PgUser> users;
    std::vector<PgDatabase> databases;
    std::size_t max_active_queries = 8;
    std::uint32_t query_timeout_ms = 30000;
};

// PostgreSQL protocol 3.0 over a POSIX socket, sharing an existing
// DuckDB instance. The caller must keep the database alive until Stop returns.
class PgWireServer {
public:
    PgWireServer(duckdb::DuckDB &database, std::string bind_host,
                 std::uint16_t port, std::string password,
                 std::size_t max_active_queries = 8, std::uint32_t query_timeout_ms = 30000,
                 std::vector<PgDatabase> databases = {});
    PgWireServer(duckdb::DuckDB &database, PgWireOptions options);
    ~PgWireServer();
    PgWireServer(const PgWireServer &) = delete;
    PgWireServer &operator=(const PgWireServer &) = delete;
    PgWireServer(PgWireServer &&) = delete;
    PgWireServer &operator=(PgWireServer &&) = delete;

    void Start();
    void Stop() noexcept;
    std::uint16_t Port() const noexcept;
    std::string Endpoint() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace duckonwire
