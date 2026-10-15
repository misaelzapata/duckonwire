#include "duckdb.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckonwire/json.hpp"
#include "duckonwire/native_access.hpp"
#include "duckonwire/pgwire.hpp"
#include "duckonwire/read_copy.hpp"
#include "quack_security_test.hpp"
#include "read_copy_pg_test.hpp"

#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#ifdef __APPLE__
#include <libproc.h>
#endif

namespace {
struct Assertion : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool condition, const std::string &message) { if (!condition) throw Assertion(message); }
std::string SafeDiagnostic(const std::exception &failure) {
    const std::string message = failure.what();
    for (const auto safe : {"Native fixture query failed", "Restricted native SQL succeeded", "Native SQL failed without an authorization denial",
            "TLS protocol receive failed", "TLS protocol send failed", "Backend disconnected before response completion",
            "Protocol receive failed or timed out", "Protocol send failed or timed out", "Cannot connect to protocol fixture",
            "Protected login failed", "Protocol login failed", "Missing SCRAM mechanism challenge", "Fixture TLS hostname verification failed"})
        if (message == safe) return safe;
    if (dynamic_cast<const std::filesystem::filesystem_error *>(&failure)) return "Owned fixture filesystem operation failed";
    return "Fixture operation failed without safe diagnostic";
}
using quack_security_test::SafeCheck;
using quack_security_test::LocalEngine;
namespace pg = read_copy_pg_test;
namespace fs = std::filesystem;
constexpr std::int64_t row_count = 4096;
const std::string snapshot_sql = "SELECT min(g)::BIGINT,max(g)::BIGINT,count(*)::BIGINT,sum(v)::BIGINT,sum(id)::BIGINT FROM sample";

std::int64_t Snapshot(const std::vector<std::int64_t> &values) {
    Require(values.size() == 5, "Snapshot shape changed");
    const auto generation = values[0];
    Require(values[1] == generation, "Snapshot contains mixed generations");
    Require(values[2] == row_count && values[3] == row_count * generation * 100000 + row_count * (row_count - 1) / 2 &&
            values[4] == row_count * (row_count - 1) / 2, "Snapshot count or checksum changed");
    return generation;
}
std::int64_t Snapshot(pg::Wire &wire) {
    const auto replies = wire.Query(snapshot_sql);
    Require(!pg::Contains(replies, 'E'), "Protected snapshot query failed");
    const auto rows = pg::Rows(replies);
    Require(rows.size() == 1 && rows[0].size() == 5, "Protected snapshot shape changed");
    std::vector<std::int64_t> values;
    for (const auto &value : rows[0]) { Require(bool(value), "Snapshot contains null"); values.push_back(std::stoll(*value)); }
    return Snapshot(values);
}
std::int64_t Snapshot(duckdb::Connection &connection) {
    auto result = connection.Query(snapshot_sql); SafeCheck(result);
    const auto rows = test_support::FetchRows(*result);
    Require(rows.size() == 1 && rows[0].size() == 5, "Native snapshot shape changed");
    std::vector<std::int64_t> values;
    for (const auto &value : rows[0]) values.push_back(value.GetValue<std::int64_t>());
    return Snapshot(values);
}
void Seed(const fs::path &path, std::int64_t generation, const std::string &extra = {}) {
    fs::create_directories(path.parent_path());
    {
        auto database = LocalEngine(path.string()); duckdb::Connection connection(*database);
        SafeCheck(connection.Query("CREATE TABLE sample AS SELECT " + std::to_string(generation) +
            "::BIGINT AS g,i::BIGINT AS id,(i+" + std::to_string(generation * 100000) +
            ")::BIGINT AS v FROM range(" + std::to_string(row_count) + ") t(i)"));
        SafeCheck(connection.Query("CREATE VIEW sample_view AS SELECT * FROM sample"));
        SafeCheck(connection.Query("CREATE SEQUENCE fixture_sequence"));
        if (!extra.empty()) SafeCheck(connection.Query(extra));
        SafeCheck(connection.Query("CHECKPOINT"));
    }
    Require(chmod(path.c_str(), 0400) == 0, "Cannot protect owned generation file");
    Require(!fs::exists(path.string() + ".wal"), "Generation retained a WAL");
}
void AtomicText(const fs::path &path, const std::string &text, mode_t mode = 0600) {
    const auto next = fs::path(path.string() + ".next");
    Require(!fs::exists(next), "Owned pointer staging path already exists");
    {
        std::ofstream output(next, std::ios::binary); output << text;
        Require(bool(output), "Cannot write owned pointer fixture");
    }
    Require(chmod(next.c_str(), mode) == 0, "Cannot protect owned pointer fixture");
    fs::rename(next, path);
}
std::string FileHash(const fs::path &path) {
    std::ifstream input(path, std::ios::binary); Require(bool(input), "Cannot hash owned fixture");
    auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    Require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr), "Cannot initialize fixture hash");
    char buffer[16384];
    while (input.read(buffer, sizeof(buffer)) || input.gcount())
        Require(EVP_DigestUpdate(context.get(), buffer, static_cast<std::size_t>(input.gcount())), "Cannot hash fixture bytes");
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned size = 0;
    Require(input.eof() && EVP_DigestFinal_ex(context.get(), digest, &size) && size == 32, "Cannot finish fixture hash");
    std::string value; static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < size; ++i) { value += hex[digest[i] >> 4]; value += hex[digest[i] & 15]; }
    return value;
}
std::int64_t OpenHandles(pid_t pid, const fs::path &path) {
    std::int64_t count = 0;
#ifdef __APPLE__
    const auto bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    Require(bytes > 0, "Cannot inspect owned server descriptors");
    std::vector<proc_fdinfo> descriptors(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
    const auto read = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, descriptors.data(),
        static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
    Require(read > 0, "Cannot enumerate owned server descriptors");
    descriptors.resize(static_cast<std::size_t>(read) / sizeof(proc_fdinfo));
    for (const auto &descriptor : descriptors) {
        if (descriptor.proc_fdtype != PROX_FDTYPE_VNODE) continue;
        vnode_fdinfowithpath info {};
        if (proc_pidfdinfo(pid, descriptor.proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info)) == sizeof(info) &&
            path == fs::path(info.pvip.vip_path)) ++count;
    }
#else
    for (const auto &entry : fs::directory_iterator("/proc/" + std::to_string(pid) + "/fd")) {
        std::error_code error; const auto target = fs::read_symlink(entry.path(), error);
        if (!error && target == path) ++count;
    }
#endif
    return count;
}

class Fixture {
public:
    Fixture(const fs::path &parent, const fs::path &executable, bool protected_mode = true, std::string *stage = nullptr)
        : root(parent), executable(executable), protected_mode(protected_mode), password(quack_security_test::RandomPassword()), stage(stage) {
        Stage("read_copy_seed_immutable_generations");
        certificate = root.path() / "ca.pem"; pointer = root.path() / "active.pointer";
        fs::create_directory(Versions()); Require(chmod(Versions().c_str(), 0700) == 0, "Cannot protect owned version directory");
        for (int generation = 1; generation <= 24; ++generation) {
            Seed(Generation(generation), generation);
            hashes.emplace(Generation(generation), FileHash(Generation(generation)));
        }
        Stage("read_copy_seed_ordinary_database");
        Seed(root.path() / "primary.duckdb", 100);
        Require(chmod((root.path() / "primary.duckdb").c_str(), 0600) == 0, "Cannot enable owned ordinary database writes");
        AtomicText(pointer, Relative(1) + '\n');
        arguments = {"--read-copy", "reporting=active.pointer", "--max-connections-per-minute", "10000",
            "--max-requests-per-minute", "100000", "--auth-max-failures", "100"};
        if (protected_mode) {
            Stage("read_copy_private_tls_and_rules_setup");
            const auto key = root.path() / "key.pem", access = root.path() / "access.tsv";
            quack_security_test::Certificate(certificate, key);
            std::ofstream rules(access);
            const auto verifier = duckonwire::MakeScramVerifier(password, 4096);
            for (const auto name : {"duckonwire", "reporting"})
                rules << "database\t" << name << "\nuser\treader\tread\t" << verifier
                      << "\nuser\twriter\twrite\t" << verifier << '\n';
            rules.close(); Require(bool(rules) && chmod(access.c_str(), 0600) == 0, "Cannot persist private read-copy rules");
            arguments.insert(arguments.end(), {"--access-file", access.string(), "--tls-cert", certificate.string(), "--tls-key", key.string()});
        }
        Start();
    }
    void Start() {
        Stage("read_copy_owned_server_startup");
        try {
            server = std::make_unique<test_support::NativeServer>(root.path(), root.path() / "primary.duckdb", executable,
                0, false, arguments, 2);
        } catch (const std::exception &failure) {
            const std::string message = failure.what();
            // Match fixed owner diagnostics only. Raw startup output may contain
            // the private descriptor token and must never enter a result.
            for (const auto safe : {"Invalid private read-copy publication", "Read-copy catalog operation failed",
                    "Read-copy catalog verification failed", "Protected database contains an unsafe or unverifiable stored SQL definition",
                    "This server requires the integrated native DuckDB build",
                    "Protected PGwire permits only the published database and its native WAL files",
                    "Protected PGwire permits only the published database's native spill directory",
                    "Database, WAL/recovery, temporary, spill, secrets, descriptor and configuration paths must not overlap"})
                if (message.find(safe) != std::string::npos) throw Assertion(safe);
            throw Assertion("Owned server did not reach verified readiness");
        }
    }
    void Stop() { server->stop(); Require(server->clean_shutdown(), "Owned read-copy server did not stop gracefully"); }
    fs::path Versions() const { return fs::path(pointer.string() + ".versions"); }
    std::string Relative(int generation) const { return "g" + std::to_string(generation) + ".duckdb"; }
    fs::path Generation(int generation) const { return Versions() / Relative(generation); }
    std::unique_ptr<pg::Wire> PostgreSQL(const std::string &role = "reader", const std::string &database = "reporting") {
        auto client = std::make_unique<pg::Wire>(server->pg_port());
        if (protected_mode) client->ProtectedLogin(role, password, certificate, database);
        else client->Login(server->token(), database);
        return client;
    }
    std::unique_ptr<quack_security_test::Client> Native(const std::string &role = "reader") {
        auto client = std::make_unique<quack_security_test::Client>();
        const auto &endpoint = server->endpoint(); const auto colon = endpoint.rfind(':');
        Require(colon != std::string::npos, "Owned read-copy endpoint malformed");
        client->Attach(endpoint.substr(0, colon + 1) + std::to_string(std::stoi(endpoint.substr(colon + 1)) + 1),
            protected_mode ? role + ':' + password : server->token());
        client->Connect(); return client;
    }
    bool DescriptorHasGeneration(int generation) const {
        std::ifstream input(server->state()); std::ostringstream data; data << input.rdbuf();
        return data.str().find("\"path\":" + duckonwire::JsonString(Generation(generation).string())) != std::string::npos;
    }
    void Wait(int generation, pg::Wire &reader) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (Snapshot(reader) == generation && DescriptorHasGeneration(generation)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Read-copy publication missed deadline");
    }
    void Publish(int generation, pg::Wire &reader) { AtomicText(pointer, Relative(generation) + '\n'); Wait(generation, reader); }
    void Preserved(int generation, pg::Wire &reader) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
        do {
            Require(Snapshot(reader) == generation && DescriptorHasGeneration(generation), "Rejected refresh changed active generation");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
    }
    test_support::TempRoot root;
    std::unique_ptr<test_support::NativeServer> server;
    fs::path certificate, pointer;
    std::map<fs::path, std::string> hashes;
private:
    fs::path executable;
    bool protected_mode;
    std::string password;
    std::vector<std::string> arguments;
    std::string *stage;
    void Stage(const std::string &value) { if (stage) *stage = value; }
};

void PGDenied(pg::Wire &wire, const std::string &sql) {
    const auto result = wire.Query(sql);
    Require(pg::HasError(result, "42501") || pg::HasError(result, "25006"), "Read-copy SQL lacked a permission denial");
}
void NativeRPCDenied(quack_security_test::Client &client, const std::string &sql) {
    // CONNECT keeps some USE/SET/ATTACH operations on the client. An explicit
    // query RPC proves the owner's policy rather than local client behavior.
    SafeCheck(client.connection->Query("DISCONNECT"));
    quack_security_test::AuthorizationDenied(client.connection->Query(
        "SELECT * FROM remote.query(" + test_support::Literal(sql) + ")"));
    client.Connect();
}
void PGPrepared(pg::Wire &wire, const std::string &name, const std::string &sql) {
    wire.Parse(name, sql); wire.Message('S');
    Require(!pg::Contains(wire.Until("Z"), 'E'), "Read-copy prepared SELECT setup failed");
}
std::int64_t PGExecute(pg::Wire &wire, const std::string &name) {
    wire.Bind("", name, {std::string("0")}); wire.Execute(""); wire.Message('S');
    const auto replies = wire.Until("Z"); Require(!pg::Contains(replies, 'E'), "Read-copy prepared SELECT failed");
    const auto rows = pg::Rows(replies);
    Require(rows.size() == 1 && rows[0].size() == 1 && rows[0][0], "Read-copy prepared SELECT shape changed");
    return std::stoll(*rows[0][0]);
}
void PGRowStream(const std::vector<pg::Reply> &replies, std::int64_t generation, std::int64_t &rows, std::int64_t &sum) {
    Require(!pg::Contains(replies, 'E'), "Read-copy pending portal failed");
    for (const auto &row : pg::Rows(replies)) {
        Require(row.size() == 3 && row[0] && row[1] && row[2] && std::stoll(*row[0]) == generation,
            "Read-copy pending portal mixed generations");
        ++rows; sum += std::stoll(*row[2]);
    }
}

struct Check { std::string name; bool passed; std::string reason; };
} // namespace

int main(int argc, char **argv) {
    fs::path executable, parent = fs::temp_directory_path();
    bool controller_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 == argc) return 2;
        if (option == "--server") executable = argv[++i];
        else if (option == "--fixture-parent") parent = argv[++i];
        else if (option == "--mode") { const std::string mode = argv[++i]; if (mode != "controller") return 2; controller_only = true; }
        else return 2;
    }
    std::vector<Check> checks; std::map<std::string, std::int64_t> metrics;
    std::set<std::string> names;
    std::string phase = "read_copy_fixture_setup";
    auto test = [&](const std::string &name, const std::function<void()> &body) {
        phase = name;
        Require(names.insert(name).second, "Duplicate read-copy check name");
        try { body(); checks.push_back({name, true}); }
        catch (const Assertion &failure) { checks.push_back({name, false, failure.what()}); }
        catch (const std::exception &failure) { checks.push_back({name, false, phase + ": " + SafeDiagnostic(failure)}); }
        catch (...) { checks.push_back({name, false}); }
    };
    try {
        Require(controller_only || !executable.empty(), "--server is required");
        if (!controller_only) {
        Fixture fixture(parent, executable, true, &phase);
        phase = "read_copy_protected_pg_session_setup";
        auto existing = fixture.PostgreSQL(), old_pg = fixture.PostgreSQL(), prepared_pg = fixture.PostgreSQL();
        phase = "read_copy_protected_native_session_setup";
        auto native = fixture.Native(), old_native = fixture.Native(), prepared_native = fixture.Native();
        phase = "read_copy_writer_and_ordinary_session_setup";
        auto writer = fixture.PostgreSQL("writer"); auto native_writer = fixture.Native("writer");
        auto ordinary = fixture.PostgreSQL("writer", "duckonwire");
        phase = "read_copy_native_prepared_setup";
        auto prepared = prepared_native->connection->Prepare("SELECT min(g)::BIGINT FROM sample WHERE id>=?");
        SafeCheck(prepared);
        auto native_execute = [&] {
            auto result = prepared->Execute(duckdb::Value::BIGINT(0)); SafeCheck(result);
            const auto rows = test_support::FetchRows(*result);
            Require(rows.size() == 1 && rows[0].size() == 1, "Native prepared result shape changed");
            return rows[0][0].GetValue<std::int64_t>();
        };
        test("protected_pg_default_catalog_reads_initial_generation", [&] { Require(Snapshot(*existing) == 1, "Initial PG generation changed"); });
        test("protected_native_default_catalog_reads_initial_generation", [&] { Require(Snapshot(*native->connection) == 1, "Initial native generation changed"); });
        test("protected_both_prepared_selects_read_initial_generation", [&] {
            PGPrepared(*prepared_pg, "generation", "SELECT min(g)::BIGINT FROM sample WHERE id >= $1::BIGINT");
            Require(PGExecute(*prepared_pg, "generation") == 1 && native_execute() == 1, "Initial prepared generation changed");
        });
        Require(!pg::Contains(old_pg->Query("BEGIN"), 'E') && Snapshot(*old_pg) == 1, "Cannot pin old PG generation");
        old_native->connection->BeginTransaction(); Require(Snapshot(*old_native->connection) == 1, "Cannot pin old native generation");
        const auto original_pid = fixture.server->pid(), original_pg_port = fixture.server->pg_port();
        const auto original_endpoint = fixture.server->endpoint();
        test("host_pointer_publication_keeps_pid_and_listeners", [&] {
            fixture.Publish(2, *existing);
            Require(fixture.server->pid() == original_pid && fixture.server->pg_port() == original_pg_port &&
                fixture.server->endpoint() == original_endpoint && kill(original_pid, 0) == 0, "Publication restarted owned listeners");
        });
        test("accessed_pg_transaction_retains_old_generation", [&] { Require(Snapshot(*old_pg) == 1, "Old PG transaction moved"); });
        test("accessed_native_transaction_retains_old_generation", [&] { Require(Snapshot(*old_native->connection) == 1, "Old native transaction moved"); });
        test("old_generation_handle_retained_while_transaction_active", [&] { Require(OpenHandles(original_pid, fixture.Generation(1)) >= 1, "Pinned generation handle absent"); });
        test("existing_autocommit_pg_and_native_read_new_generation", [&] {
            Require(Snapshot(*existing) == 2 && Snapshot(*native->connection) == 2, "Existing session remained stale");
        });
        test("new_pg_and_native_sessions_read_new_generation", [&] {
            auto fresh_pg = fixture.PostgreSQL(); auto fresh_native = fixture.Native();
            Require(Snapshot(*fresh_pg) == 2 && Snapshot(*fresh_native->connection) == 2, "New session selected stale generation");
        });
        test("existing_pg_and_native_prepared_selects_rebind", [&] {
            Require(PGExecute(*prepared_pg, "generation") == 2 && native_execute() == 2, "Prepared session remained stale");
        });
        test("old_transactions_finish_and_same_sessions_read_new_generation", [&] {
            Require(!pg::Contains(old_pg->Query("COMMIT"), 'E'), "Old PG commit failed");
            old_native->connection->Commit();
            Require(Snapshot(*old_pg) == 2 && Snapshot(*old_native->connection) == 2, "Finished transaction retained retired generation");
        });
        auto portal = fixture.PostgreSQL(); Require(!pg::Contains(portal->Query("BEGIN"), 'E'), "Cannot begin pending portal");
        PGPrepared(*portal, "stream", "SELECT g,id,v FROM sample ORDER BY id");
        portal->Bind("stream", "stream"); portal->Execute("stream", 100); portal->Message('S');
        const auto first_rows = portal->Until("Z"); Require(pg::Contains(first_rows, 's'), "Pending portal did not suspend");
        auto stream_client = fixture.Native(); auto stream = stream_client->connection->SendQuery("SELECT g,id,v FROM sample ORDER BY id");
        SafeCheck(stream); auto first_chunk = stream->Fetch(); Require(bool(first_chunk), "Native stream has no initial chunk");
        fixture.Publish(3, *existing);
        test("pg_pending_portal_finishes_old_generation_after_publication", [&] {
            std::int64_t rows = 0, sum = 0; PGRowStream(first_rows, 2, rows, sum);
            portal->Execute("stream"); portal->Message('S'); PGRowStream(portal->Until("Z"), 2, rows, sum);
            Require(rows == row_count && sum == row_count * 2 * 100000 + row_count * (row_count - 1) / 2, "Pending portal count changed");
            portal->CloseObject('P', "stream"); portal->Message('S');
            Require(!pg::Contains(portal->Until("Z"), 'E') && !pg::Contains(portal->Query("COMMIT"), 'E') && Snapshot(*portal) == 3,
                "Pending portal cleanup retained old generation");
        });
        test("native_pending_result_finishes_old_generation_after_publication", [&] {
            std::int64_t rows = 0, sum = 0;
            auto inspect = [&](duckdb::DataChunk &chunk) {
                for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
                    Require(chunk.GetValue(0, row).GetValue<std::int64_t>() == 2, "Native pending result mixed generations");
                    ++rows; sum += chunk.GetValue(2, row).GetValue<std::int64_t>();
                }
            };
            inspect(*first_chunk); first_chunk.reset(); while (auto chunk = stream->Fetch()) inspect(*chunk);
            Require(rows == row_count && sum == row_count * 2 * 100000 + row_count * (row_count - 1) / 2, "Native pending result count changed");
            stream.reset(); Require(Snapshot(*stream_client->connection) == 3, "Native stream cleanup retained old generation");
        });
        test("read_copy_writer_grants_deny_pg_and_native_mutations", [&] {
            for (const auto sql : {"INSERT INTO sample VALUES(999,999,999)", "UPDATE sample SET v=0", "DELETE FROM sample", "CREATE TABLE forbidden(i INTEGER)",
                    "CREATE TEMP TABLE forbidden_temp(i INTEGER)", "SELECT nextval('fixture_sequence')"}) {
                PGDenied(*writer, sql); quack_security_test::Denied(*native_writer->connection, sql);
            }
            Require(Snapshot(*writer) == 3 && Snapshot(*native_writer->connection) == 3, "Denied write changed published rows");
        });
        test("read_copy_prepared_mutations_denied_for_writer_grants", [&] {
            writer->Parse("denied", "UPDATE sample SET v=$1::BIGINT WHERE id=0"); writer->Bind("", "denied", {std::string("7")});
            writer->Execute(""); writer->Message('S'); Require(pg::HasError(writer->Until("Z"), "42501"), "PG prepared mutation lacked denial");
            quack_security_test::AuthorizationDenied(native_writer->connection->Prepare("UPDATE sample SET v=? WHERE id=0"));
        });
        test("read_copy_catalog_configuration_and_host_control_bypass_denied", [&] {
            for (const auto sql : {"USE memory", "DETACH base", "SET enable_external_access=true", "SET lock_configuration=false", "ATTACH ':memory:' AS bypass", "CALL quack_stop('quack:127.0.0.1:1')"}) {
                PGDenied(*writer, sql); NativeRPCDenied(*native_writer, sql);
            }
            Require(Snapshot(*writer) == 3 && Snapshot(*native_writer->connection) == 3, "Denied catalog change affected default base");
        });
        test("read_copy_storage_exception_does_not_permit_remote_file_reads", [&] {
            for (const auto &path : {fixture.Generation(4), fixture.root.path() / "access.tsv", fixture.root.path() / "primary.duckdb"}) {
                for (const auto function : {"read_blob", "read_csv", "sniff_csv", "parquet_metadata", "parquet_schema"}) {
                    const auto sql = "SELECT * FROM " + std::string(function) + "(" + test_support::Literal(path.string()) + ")";
                    PGDenied(*writer, sql); NativeRPCDenied(*native_writer, sql);
                }
            }
        });
        test("ordinary_database_remains_read_write_and_isolated", [&] {
            Require(Snapshot(*ordinary) == 100 && !pg::Contains(ordinary->Query("CREATE TABLE ordinary_write(value INTEGER)"), 'E') &&
                !pg::Contains(ordinary->Query("INSERT INTO ordinary_write VALUES(42)"), 'E') && pg::Scalar(*ordinary, "SELECT value FROM ordinary_write") == 42,
                "Read-copy mode affected ordinary database writes");
            Require(pg::Contains(existing->Query("SELECT * FROM duckonwire.main.sample"), 'E'), "Read-copy session reached another engine catalog");
        });
        const auto invalid = [&](const std::string &text) { AtomicText(fixture.pointer, text); fixture.Preserved(3, *existing); };
        test("missing_candidate_preserves_last_good_without_creating_file", [&] {
            invalid("missing.duckdb\n"); Require(!fs::exists(fixture.Versions() / "missing.duckdb"), "Missing immutable candidate was created");
        });
        test("corrupt_candidate_preserves_last_good", [&] {
            const auto corrupt = fixture.Versions() / "corrupt.duckdb"; AtomicText(corrupt, "owned invalid database", 0400); invalid("corrupt.duckdb\n");
        });
        test("malformed_pointer_preserves_last_good", [&] { invalid("g4.duckdb\ng5.duckdb\n"); });
        test("traversal_and_absolute_candidate_paths_preserve_last_good", [&] {
            invalid("../active.pointer.versions/g4.duckdb\n"); invalid(fixture.Generation(4).string() + '\n'); invalid("../outside.duckdb\n");
        });
        test("symlink_candidate_and_parent_preserve_last_good", [&] {
            fs::create_symlink(fixture.Generation(4), fixture.Versions() / "link.duckdb"); invalid("link.duckdb\n");
            fs::create_directory_symlink(fixture.Versions(), fixture.root.path() / "linked"); invalid("../linked/g4.duckdb\n");
            fs::create_hard_link(fixture.Generation(4), fixture.Versions() / "hard.duckdb"); invalid("hard.duckdb\n");
            fs::remove(fixture.Versions() / "hard.duckdb");
            test_support::TempRoot outside(parent); Seed(outside.path() / "outside.duckdb", 800);
            fs::create_symlink(outside.path() / "outside.duckdb", fixture.Versions() / "external.duckdb"); invalid("external.duckdb\n");
        });
        test("writable_candidate_and_wal_candidate_preserve_last_good", [&] {
            Require(chmod(fixture.Generation(4).c_str(), 0600) == 0, "Cannot prepare writable candidate"); invalid("g4.duckdb\n");
            Require(chmod(fixture.Generation(4).c_str(), 0400) == 0, "Cannot restore candidate immutability");
            AtomicText(fs::path(fixture.Generation(5).string() + ".wal"), "owned invalid WAL"); invalid("g5.duckdb\n");
            fs::remove(fixture.Generation(5).string() + ".wal");
        });
        test("nonprivate_and_symlink_pointer_preserve_last_good", [&] {
            phase = "nonprivate_pointer_policy_preserved";
            AtomicText(fixture.pointer, "g4.duckdb\n", 0644); fixture.Preserved(3, *existing);
            phase = "symlink_pointer_fixture_setup";
            const auto alternate = fixture.root.path() / "alternate.pointer"; AtomicText(alternate, "g4.duckdb\n");
            fs::remove(fixture.pointer); fs::create_symlink(alternate, fixture.pointer);
            phase = "symlink_pointer_policy_preserved"; fixture.Preserved(3, *existing);
            phase = "symlink_pointer_fixture_restore";
            AtomicText(fixture.pointer, "g3.duckdb\n");
        });
        test("unsafe_stored_view_macro_and_default_candidates_audited_before_publish", [&] {
            const std::vector<std::string> definitions {
                "CREATE VIEW hidden AS SELECT quack_connection_id('unopened_fixture_catalog') AS private_payload",
                "CREATE MACRO hidden() AS quack_connection_id('unopened_fixture_catalog')",
                "CREATE TABLE hidden(i VARCHAR DEFAULT quack_connection_id('unopened_fixture_catalog'))"};
            for (std::size_t i = 0; i < definitions.size(); ++i) {
                phase = "unsafe_definition_seed_" + std::to_string(i);
                const auto relative = "unsafe" + std::to_string(i) + ".duckdb";
                Seed(fixture.Versions() / relative, 900 + static_cast<std::int64_t>(i), definitions[i]);
                phase = "unsafe_definition_policy_" + std::to_string(i); invalid(relative + '\n');
            }
        });
        AtomicText(fixture.pointer, "g3.duckdb\n");
        test("retired_generation_handles_drain_after_sessions_and_results_finish", [&] {
            portal.reset(); stream_client.reset(); old_pg.reset(); old_native.reset(); prepared.reset(); prepared_native.reset();
            Require(PGExecute(*prepared_pg, "generation") == 3 && Snapshot(*native->connection) == 3, "Cannot release stale prepared generation");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!OpenHandles(original_pid, fixture.Generation(1)) && !OpenHandles(original_pid, fixture.Generation(2))) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            throw std::runtime_error("Retired immutable file handles accumulated");
        });
        test("four_readers_twenty_swaps_have_zero_mixed_generations_or_errors", [&] {
            std::atomic<bool> stop {false}; std::atomic<unsigned> ready {0};
            std::atomic<std::int64_t> reads {0}, errors {0}; std::atomic<std::int64_t> observed[4];
            for (auto &value : observed) value = 0;
            std::vector<std::thread> threads;
            auto worker = [&](unsigned worker) {
                try {
                    std::int64_t previous = 0;
                    if (worker < 2) {
                        auto reader = fixture.PostgreSQL(); ++ready;
                        while (!stop) {
                            Require(!pg::Contains(reader->Query("BEGIN"), 'E'), "Stress PG BEGIN failed");
                            const auto a = Snapshot(*reader); std::this_thread::sleep_for(std::chrono::milliseconds(1)); const auto b = Snapshot(*reader);
                            Require(a == b && a >= previous && !pg::Contains(reader->Query("COMMIT"), 'E'), "Stress PG snapshot drifted");
                            previous = a; observed[worker] = a; ++reads;
                        }
                    } else {
                        auto reader = fixture.Native(); ++ready;
                        while (!stop) {
                            reader->connection->BeginTransaction(); const auto a = Snapshot(*reader->connection);
                            std::this_thread::sleep_for(std::chrono::milliseconds(1)); const auto b = Snapshot(*reader->connection);
                            Require(a == b && a >= previous, "Stress native snapshot drifted"); reader->connection->Commit();
                            previous = a; observed[worker] = a; ++reads;
                        }
                    }
                } catch (...) { ++errors; stop = true; }
            };
            std::exception_ptr publication_error;
            try {
                for (unsigned i = 0; i < 4; ++i) threads.emplace_back(worker, i);
                const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (ready < 4 && !stop && std::chrono::steady_clock::now() < startup_deadline) std::this_thread::yield();
                Require(ready == 4 && !stop, "Stress readers missed startup deadline");
                for (int generation = 4; generation <= 23; ++generation) {
                    fixture.Publish(generation, *existing);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!stop && std::chrono::steady_clock::now() < deadline) {
                        bool all = true; for (auto &value : observed) all = all && value >= generation;
                        if (all) break; std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    for (auto &value : observed) Require(value >= generation, "Stress reader missed a published generation");
                    Require(Snapshot(*ordinary) == 100 && pg::Scalar(*ordinary, "SELECT value FROM ordinary_write") == 42,
                        "Read-copy publication affected ordinary engine");
                    ++metrics["concurrent_swaps"];
                }
            } catch (...) { publication_error = std::current_exception(); }
            stop = true; for (auto &thread : threads) if (thread.joinable()) thread.join();
            metrics["concurrent_transaction_reads"] = reads; metrics["concurrent_reader_errors"] = errors;
            Require(!publication_error && errors == 0 && metrics["concurrent_swaps"] == 20 && reads >= 80, "Concurrent read-copy acceptance failed");
        });
        test("immutable_candidate_file_hashes_remain_unchanged", [&] {
            for (const auto &entry : fixture.hashes) Require(FileHash(entry.first) == entry.second, "Read-only generation bytes changed");
        });
        test("restart_reads_last_acknowledged_generation_and_ordinary_data", [&] {
            existing.reset(); native.reset(); prepared_pg.reset(); writer.reset(); native_writer.reset(); ordinary.reset();
            fixture.Stop(); fixture.Start();
            auto restarted = fixture.PostgreSQL(); auto restarted_native = fixture.Native(); auto other = fixture.PostgreSQL("writer", "duckonwire");
            Require(Snapshot(*restarted) == 23 && Snapshot(*restarted_native->connection) == 23 &&
                pg::Scalar(*other, "SELECT value FROM ordinary_write") == 42 && fixture.DescriptorHasGeneration(23), "Restart lost acknowledged immutable generation");
        });
        test("owned_protected_fixture_shuts_down_gracefully", [&] { fixture.Stop(); });
        phase = "local_read_copy_fixture_setup";
        Fixture local(parent, executable, false, &phase);
        test("local_shared_token_read_copy_denies_pg_and_native_writes_and_catalog_bypasses", [&] {
            auto pg_client = local.PostgreSQL(); auto native_client = local.Native();
            Require(Snapshot(*pg_client) == 1 && Snapshot(*native_client->connection) == 1, "Local read-copy default catalog failed");
            for (const auto sql : {"INSERT INTO sample VALUES(999,999,999)", "CREATE TEMP TABLE forbidden(i INTEGER)", "SELECT nextval('fixture_sequence')",
                    "USE memory", "SET enable_external_access=true", "ATTACH ':memory:' AS bypass"}) {
                PGDenied(*pg_client, sql); NativeRPCDenied(*native_client, sql);
            }
            local.Publish(2, *pg_client); Require(Snapshot(*native_client->connection) == 2, "Local native read-copy session remained stale");
        });
        test("owned_local_fixture_shuts_down_gracefully", [&] { local.Stop(); });
        }
        phase = "read_copy_fault_fixture_setup";
        test_support::TempRoot fault_root(parent);
        const auto fault_pointer = fault_root.path() / "fault.pointer";
        const auto fault_versions = fs::path(fault_pointer.string() + ".versions");
        fs::create_directory(fault_versions); Require(chmod(fault_versions.c_str(), 0700) == 0, "Cannot protect fault fixture storage");
        for (int generation = 1; generation <= 3; ++generation)
            Seed(fault_versions / ("g" + std::to_string(generation) + ".duckdb"), generation);
        AtomicText(fault_pointer, "g1.duckdb\n");
        auto fault_engine = [&] {
            duckdb::DBConfig config;
            config.SetOptionByName("threads", duckdb::Value::BIGINT(2));
            config.SetOptionByName("memory_limit", duckdb::Value("64MiB"));
            config.SetOptionByName("enable_external_access", duckdb::Value(false));
            config.SetOptionByName("allow_unsigned_extensions", duckdb::Value(false));
            config.SetOptionByName("allow_persistent_secrets", duckdb::Value(false));
            auto database = std::make_unique<duckdb::DuckDB>(nullptr, &config);
            duckdb::DBConfig::GetConfig(*database->instance).AddAllowedDirectory(fault_versions.string());
            return database;
        };
        auto host = fault_engine(); bool rollback_hazard_seen = false; std::string rollback_stage = "hook_not_called";
        auto controller = std::make_unique<duckonwire::ReadCopyController>(*host, fault_root.path(), fault_pointer,
            duckonwire::ReadCopyController::PathCheck {}, [&](duckdb::Connection &owner) {
                // Reproduce the paired engine's known ATTACH rollback hazard.
                rollback_stage = "begin";
                SafeCheck(owner.Query("BEGIN"));
                rollback_stage = "replace";
                SafeCheck(owner.Query("ATTACH OR REPLACE " + test_support::Literal((fault_versions / "g3.duckdb").string()) +
                    " AS base (READ_ONLY)"));
                rollback_stage = "rollback";
                SafeCheck(owner.Query("ROLLBACK"));
                rollback_stage = "inspect_alias";
                // SQL cannot bind while its detached default catalog is absent.
                rollback_hazard_seen = !duckdb::DatabaseManager::Get(*owner.context).GetDatabase(duckdb::Identifier("base"));
                rollback_stage = "throw_after_inspection";
                throw std::runtime_error("Owned postpublication verification fault");
            });
        {
            duckdb::Connection owner(*host); SafeCheck(owner.Query("SET lock_configuration=true"));
        }
        test("postpublication_rollback_hazard_explicitly_restores_last_good", [&] {
            AtomicText(fault_pointer, "g2.duckdb\n");
            const auto recovery = controller->Poll();
            metrics["fault_recovery_result"] = static_cast<std::int64_t>(recovery);
            metrics["fault_rollback_hazard_seen"] = rollback_hazard_seen;
            metrics["fault_generation"] = controller->Generation();
            metrics["fault_last_good_path_retained"] = controller->Path() == fault_versions / "g1.duckdb";
            metrics["fault_route_available"] = controller->Available()->load();
            Require(recovery == duckonwire::ReadCopyController::Result::Restored, "Postpublication fault did not return restored status");
            Require(rollback_hazard_seen, "Owned ATTACH rollback did not reproduce alias-loss hazard at " + rollback_stage);
            Require(controller->Generation() == 1 && controller->Path() == fault_versions / "g1.duckdb" && controller->Available()->load(),
                "Explicit recovery changed last-good metadata or availability");
            duckdb::Connection observer(*host);
            Require(Snapshot(observer) == 1 && quack_security_test::Scalar(observer, "SELECT current_setting('enable_external_access')::BIGINT") == 0 &&
                quack_security_test::Scalar(observer, "SELECT current_setting('lock_configuration')::BIGINT") == 1,
                "Recovery changed data or global security configuration");
        });
        test("restart_after_failed_publish_uses_durable_ack_not_pending_candidate", [&] {
            controller.reset(); host.reset(); host = fault_engine();
            controller = std::make_unique<duckonwire::ReadCopyController>(*host, fault_root.path(), fault_pointer);
            duckdb::Connection observer(*host);
            Require(Snapshot(observer) == 1 && controller->Path() == fault_versions / "g1.duckdb", "Restart selected unacknowledged candidate");
            Require(controller->Poll() == duckonwire::ReadCopyController::Result::Published && Snapshot(observer) == 2 &&
                controller->Path() == fault_versions / "g2.duckdb", "Restart did not validate pending immutable publication");
        });
        test("restart_with_malformed_pending_pointer_retains_durable_ack", [&] {
            controller.reset(); host.reset(); AtomicText(fault_pointer, "g2.duckdb\ng3.duckdb\n"); host = fault_engine();
            controller = std::make_unique<duckonwire::ReadCopyController>(*host, fault_root.path(), fault_pointer);
            duckdb::Connection observer(*host);
            Require(Snapshot(observer) == 2 && controller->Path() == fault_versions / "g2.duckdb" &&
                controller->Poll() == duckonwire::ReadCopyController::Result::Rejected && Snapshot(observer) == 2,
                "Malformed pending pointer defeated restart recovery");
        });
        controller.reset(); host.reset(); AtomicText(fault_pointer, "g2.duckdb\n"); host = fault_engine();
        controller = std::make_unique<duckonwire::ReadCopyController>(*host, fault_root.path(), fault_pointer,
            duckonwire::ReadCopyController::PathCheck {}, [&](duckdb::Connection &) {
                fs::remove(fault_versions / "g2.duckdb");
                throw std::runtime_error("Owned unrecoverable publication fault");
            });
        const auto native_token = quack_security_test::RandomPassword();
        duckonwire::ConfigureReadCopyNativeAccess(*host, native_token, controller->Available());
        duckdb::Connection native_owner(*host);
        const auto endpoint = "quack:127.0.0.1:" + std::to_string(test_support::AvailablePort());
        SafeCheck(native_owner.Query("CALL quack_serve(?, token=?)", duckdb::Value(endpoint), duckdb::Value(native_token)));
        struct StopNative {
            duckdb::Connection &owner; const std::string &endpoint;
            ~StopNative() { try { owner.Query("CALL quack_stop(?)", duckdb::Value(endpoint)); } catch (...) {} }
        } stop_native {native_owner, endpoint};
        SafeCheck(native_owner.Query("SET lock_configuration=true"));
        auto ordinary_host = LocalEngine(); duckdb::Connection ordinary_owner(*ordinary_host);
        SafeCheck(ordinary_owner.Query("CREATE TABLE ordinary_sentinel AS SELECT 42::BIGINT AS value"));
        duckonwire::PgWireServer fault_pg(*ordinary_host, "127.0.0.1", 0, native_token, 8, 30000,
            {{"duckonwire", ordinary_host.get()}, {"reporting", host.get(), true, controller->Available(), fault_versions.string()}});
        fault_pg.Start();
        pg::Wire fault_pg_client(fault_pg.Port()); fault_pg_client.Login(native_token, "reporting");
        pg::Wire unaffected_pg_client(fault_pg.Port()); unaffected_pg_client.Login(native_token, "duckonwire");
        quack_security_test::Client fault_native_client; fault_native_client.Attach(endpoint, native_token); fault_native_client.Connect();
        Require(Snapshot(fault_pg_client) == 2 && Snapshot(*fault_native_client.connection) == 2, "Fault fixture did not begin healthy");
        test("failed_last_good_restoration_blocks_affected_pg_and_native_routes_only", [&] {
            AtomicText(fault_pointer, "g3.duckdb\n");
            Require(controller->Poll() == duckonwire::ReadCopyController::Result::Unavailable && !controller->Available()->load() &&
                controller->Generation() == 1 && controller->Path() == fault_versions / "g2.duckdb", "Failed recovery was acknowledged or remained available");
            fault_pg_client.QuerySend("SELECT 42");
            const auto rejected = fault_pg_client.Until("E");
            Require(pg::HasError(rejected, "57P03") && !fault_pg_client.Next(), "Unavailable PG route served a further request");
            bool native_rejected = false;
            try { auto result = fault_native_client.connection->Query("SELECT 42"); native_rejected = result && result->HasError(); }
            catch (...) { native_rejected = true; }
            Require(native_rejected && quack_security_test::RejectCredential(endpoint, native_token), "Unavailable native route served a further request or login");
            Require(pg::Scalar(unaffected_pg_client, "SELECT value FROM ordinary_sentinel") == 42,
                "Failed read-copy recovery blocked another database");
        });
        fault_pg.Stop();
    } catch (const Assertion &failure) { checks.push_back({phase, false, failure.what()}); }
    catch (const std::exception &failure) { checks.push_back({phase, false, SafeDiagnostic(failure)}); }
    catch (...) { checks.push_back({phase, false}); }
    bool passed = !checks.empty(); for (const auto &check : checks) passed = passed && check.passed;
    std::cout << "{\"suite\":\"read_copy\",\"passed\":" << (passed ? "true" : "false")
              << ",\"native_used\":true,\"synthetic_only\":true,\"source_id\":" << duckonwire::JsonString(duckdb::DuckDB::SourceID()) << ",\"checks\":[";
    for (std::size_t i = 0; i < checks.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"name\":" << duckonwire::JsonString(checks[i].name) << ",\"passed\":" << (checks[i].passed ? "true" : "false");
        if (!checks[i].passed) std::cout << ",\"error\":\"Read-copy acceptance failed; SQL, payloads and credentials are not logged\"";
        if (!checks[i].reason.empty()) std::cout << ",\"reason\":" << duckonwire::JsonString(checks[i].reason);
        std::cout << '}';
    }
    std::cout << "],\"metrics\":{"; bool first = true;
    for (const auto &metric : metrics) { if (!first) std::cout << ','; first = false; std::cout << duckonwire::JsonString(metric.first) << ':' << metric.second; }
    std::cout << "}}\n"; return passed ? 0 : 1;
}
