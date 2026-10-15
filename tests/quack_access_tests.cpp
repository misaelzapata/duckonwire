#include "duckdb.hpp"
#include "duckonwire/json.hpp"
#include "quack_security_test.hpp"

#include <functional>
#include <future>
#include <iostream>
#include <utility>

using namespace duckdb;
using namespace quack_security_test;

namespace {
struct CheckResult { std::string name; bool passed; std::string reason; };

void Unchanged(Connection &observer) {
	Require(Scalar(observer, "SELECT count(*) FROM fixture_rows WHERE id=1 AND stock=10 AND label='primary'") == 1 &&
	        Scalar(observer, "SELECT count(*) FROM fixture_rows") == 1,
	        "Denied native operation changed synthetic rows");
}

void ReaderDenied(Fixture &fixture, Connection &observer, const std::string &sql) {
	auto reader = fixture.Login();
	Denied(*reader->connection, sql);
	Unchanged(observer);
}

bool RequestRejected(Client &client) {
	try {
		auto result = client.connection->Query("SELECT 42");
		return result && result->HasError() && result->GetError().find("Quack request rejected") != std::string::npos;
	} catch (const std::exception &error) {
		return std::string(error.what()).find("Quack request rejected") != std::string::npos;
	} catch (...) { return false; }
}

void RequireHealthy(Client &client) {
	Require(Scalar(*client.connection, "SELECT 42") == 42, "Authorized native session is unusable");
}

bool PostgreSQLTLSAccepted(int port) {
	const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	Require(socket_fd >= 0, "Cannot create owned PostgreSQL cross-protocol probe");
	struct CloseSocket { int fd; ~CloseSocket() { close(fd); } } owned {socket_fd};
	timeval timeout {2, 0};
	Require(setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
	        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
	        "Cannot bound cross-protocol probe");
#ifdef SO_NOSIGPIPE
	int no_sigpipe = 1;
	setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(static_cast<std::uint16_t>(port));
	Require(connect(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
	        "Cannot connect cross-protocol probe");
	const unsigned char ssl_request[] {0, 0, 0, 8, 4, 210, 22, 47};
	const auto written = send(socket_fd, ssl_request, sizeof(ssl_request), 0);
	if (written < 0 && (errno == EPIPE || errno == ECONNRESET)) return false;
	Require(written == sizeof(ssl_request), "Incomplete cross-protocol SSL request");
	unsigned char answer = 0;
	const auto read = recv(socket_fd, &answer, 1, 0);
	if (read == 0 || (read < 0 && errno == ECONNRESET)) return false;
	Require(read == 1, "Cross-protocol probe exceeded its receive deadline");
	return answer == 'S';
}
} // namespace

int main(int argc, char **argv) {
	fs::path executable, parent = fs::temp_directory_path();
	for (int i = 1; i < argc; ++i) {
		const std::string option = argv[i];
		if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
		if (option == "--server") executable = argv[++i];
		else if (option == "--fixture-parent") parent = argv[++i];
		else { std::cerr << "Unknown option\n"; return 2; }
	}
	std::vector<CheckResult> checks;
	std::string phase = "protected_native_fixture_setup";
	auto test = [&](const std::string &name, const std::function<void()> &body) {
		phase = name;
		try { body(); checks.push_back({name, true}); }
		catch (const std::exception &error) {
			// Only exact harness messages are eligible for results; native errors may contain SQL literals.
			const std::string message = error.what();
			std::string reason;
			for (const auto safe : {"Native fixture query failed", "Restricted native SQL succeeded",
			        "Native SQL failed without an authorization denial",
			        "Unsafe stored definition was not rejected by startup policy",
			        "Protected startup diagnostic exposed a private payload",
			        "Rejected protected startup published a descriptor",
			        "Rejected protected startup left a live native endpoint"})
				if (message == safe) reason = safe;
			checks.push_back({name, false, reason});
		}
		catch (...) { checks.push_back({name, false}); }
	};
	try {
		Require(!executable.empty(), "--server is required");
		Fixture fixture(parent, executable);
		auto writer = fixture.Login("writer");
		test("reader_attach_select_and_local_join", [&] {
			auto reader = fixture.Login("reader", "duckonwire", false);
			SafeCheck(reader->connection->Query("CREATE TABLE local_ids(id INTEGER)"));
			SafeCheck(reader->connection->Query("INSERT INTO local_ids VALUES (1)"));
			Require(Scalar(*reader->connection, "SELECT sum(r.stock) FROM remote.main.fixture_rows r JOIN local_ids l USING(id)") == 10,
			        "Protected native ATTACH did not preserve local joins");
		});
		test("reader_connect_bound_and_prepared_select", [&] {
			auto reader = fixture.Login();
			auto selected = reader->connection->Query("SELECT stock FROM fixture_rows WHERE id=?", Value::INTEGER(1));
			SafeCheck(selected);
			auto selected_rows = test_support::FetchRows(*selected);
			Require(selected_rows.size() == 1 && selected_rows[0][0].GetValue<int64_t>() == 10, "Bound native reader selected another row");
			auto prepared = reader->connection->Prepare("SELECT stock FROM fixture_rows WHERE label=?");
			SafeCheck(prepared);
			auto result = prepared->Execute(Value("primary"));
			SafeCheck(result);
			auto rows = test_support::FetchRows(*result);
			Require(rows.size() == 1 && rows[0][0].GetValue<int64_t>() == 10, "Prepared native reader selected another row");
		});
		test("reader_begin_commit_and_rollback", [&] {
			auto reader = fixture.Login();
			reader->connection->BeginTransaction();
			Unchanged(*reader->connection);
			reader->connection->Commit();
			reader->connection->BeginTransaction();
			Unchanged(*reader->connection);
			reader->connection->Rollback();
			RequireHealthy(*reader);
		});
		test("reader_create_table_denied", [&] {
			ReaderDenied(fixture, *writer->connection, "CREATE TABLE native_forbidden(id INTEGER)");
			Require(Scalar(*writer->connection, "SELECT count(*) FROM information_schema.tables WHERE table_name='native_forbidden'") == 0,
			        "Reader created a remote table");
		});
		test("reader_insert_with_bound_values_denied", [&] {
			auto reader = fixture.Login();
			AuthorizationDenied(reader->connection->Query("INSERT INTO fixture_rows VALUES (?,?,?)",
			    Value::INTEGER(2), Value::INTEGER(99), Value("bound 'value'")));
			Unchanged(*writer->connection);
		});
		test("reader_update_with_bound_values_denied", [&] {
			auto reader = fixture.Login();
			AuthorizationDenied(reader->connection->Query("UPDATE fixture_rows SET stock=? WHERE id=?",
			    Value::INTEGER(99), Value::INTEGER(1)));
			Unchanged(*writer->connection);
		});
		test("reader_delete_denied", [&] { ReaderDenied(fixture, *writer->connection, "DELETE FROM fixture_rows WHERE id=1"); });
		test("reader_prepared_mutation_denied", [&] {
			auto reader = fixture.Login();
			auto prepared = reader->connection->Prepare("UPDATE fixture_rows SET stock=? WHERE id=?");
			AuthorizationDenied(prepared);
			Unchanged(*writer->connection);
		});
		test("reader_cte_mutation_denied", [&] {
			ReaderDenied(fixture, *writer->connection,
			    "WITH proposed AS (SELECT 2 AS id, 99 AS stock, 'cte' AS label) INSERT INTO fixture_rows SELECT * FROM proposed");
		});
		test("reader_select_nextval_denied", [&] { ReaderDenied(fixture, *writer->connection, "SELECT nextval('fixture_sequence')"); });
		test("reader_prepared_nextval_denied", [&] {
			auto reader = fixture.Login();
			auto prepared = reader->connection->Prepare("SELECT nextval(?)");
			Require(bool(prepared), "No native preparation result");
			if (prepared->HasError()) AuthorizationDenied(prepared);
			else AuthorizationDenied(prepared->Execute(Value("fixture_sequence")));
		});
		test("reader_cte_nextval_denied", [&] {
			ReaderDenied(fixture, *writer->connection, "WITH value AS (SELECT nextval('fixture_sequence') AS n) SELECT n FROM value");
		});
		test("reader_explain_analyze_mutation_denied", [&] {
			ReaderDenied(fixture, *writer->connection, "EXPLAIN ANALYZE UPDATE fixture_rows SET stock=99 WHERE id=1");
		});
		test("reader_explain_analyze_nextval_denied", [&] {
			ReaderDenied(fixture, *writer->connection, "EXPLAIN ANALYZE SELECT nextval('fixture_sequence')");
		});
		test("reader_multistatement_mutation_denied", [&] {
			ReaderDenied(fixture, *writer->connection, "SELECT 42; INSERT INTO fixture_rows VALUES(2,99,'batch')");
		});
		test("reader_transaction_denial_rollback_keeps_session_usable", [&] {
			auto reader = fixture.Login();
			reader->connection->BeginTransaction();
			Denied(*reader->connection, "UPDATE fixture_rows SET stock=99");
			reader->connection->Rollback();
			Unchanged(*reader->connection);
		});
		test("reader_side_effect_denials_preserve_sequence", [&] {
			Require(Scalar(*writer->connection, "SELECT nextval('fixture_sequence')") == 1,
			        "Reader query or preparation advanced the sequence");
		});
		test("protected_configuration_changes_denied_for_both_roles", [&] {
			for (const auto &role : {std::string("reader"), std::string("writer")}) {
				auto client = fixture.Login(role);
				const auto before = Scalar(*client->connection, "SELECT current_setting('threads')");
				// CONNECT may retain client-local configuration statements. Explicit remote.query
				// proves the protected owner's policy instead of relying on local SET routing.
				client->connection->Query("SET threads=1");
				Require(Scalar(*client->connection, "SELECT current_setting('threads')") == before,
				        "Connected native client changed the protected owner's settings");
				SafeCheck(client->connection->Query("DISCONNECT"));
				for (const auto sql : {"SET threads=1", "RESET threads"})
					AuthorizationDenied(client->connection->Query("SELECT * FROM remote.query(" + test_support::Literal(sql) + ")"));
				client->Connect();
				Require(Scalar(*client->connection, "SELECT current_setting('threads')") == before,
				        "Rejected native RPC changed the protected owner's settings");
			}
		});
		test("protected_callback_changes_and_calls_denied_for_both_roles", [&] {
			for (const auto &role : {std::string("reader"), std::string("writer")}) {
				auto client = fixture.Login(role);
				Denied(*client->connection, "SET quack_authentication_function='always_true'");
				Denied(*client->connection, "SELECT * FROM quack_query('remote', 'SELECT 42')");
			}
		});
		test("protected_extension_changes_denied_for_both_roles", [&] {
			for (const auto &role : {std::string("reader"), std::string("writer")}) {
				auto client = fixture.Login(role);
				Denied(*client->connection, "INSTALL json");
				Denied(*client->connection, "LOAD json");
			}
		});
		test("protected_external_file_sql_denied_for_both_roles", [&] {
			const auto export_file = fixture.root.path() / "forbidden.csv";
			for (const auto &role : {std::string("reader"), std::string("writer")}) {
				auto client = fixture.Login(role);
				Denied(*client->connection, "COPY fixture_rows TO " + test_support::Literal(export_file.string()));
				Denied(*client->connection, "SELECT * FROM read_json_auto(" + test_support::Literal(fixture.server->state().string()) + ")");
			}
			Require(!fs::exists(export_file), "Protected native SQL wrote an external file");
		});
		test("writer_prepared_crud_and_commit", [&] {
			SafeCheck(writer->connection->Query("CREATE TABLE native_crud(id INTEGER PRIMARY KEY, stock INTEGER, label VARCHAR)"));
			writer->connection->BeginTransaction();
			auto insert = writer->connection->Prepare("INSERT INTO native_crud VALUES(?,?,?)");
			SafeCheck(insert);
			SafeCheck(insert->Execute(Value::INTEGER(1), Value::INTEGER(7), Value("quoted 'value'")));
			SafeCheck(writer->connection->Query("UPDATE native_crud SET stock=? WHERE id=?", Value::INTEGER(8), Value::INTEGER(1)));
			writer->connection->Commit();
			auto observer = fixture.Login();
			Require(Scalar(*observer->connection, "SELECT stock FROM native_crud WHERE id=1") == 8, "Native writer commit was not visible");
			SafeCheck(writer->connection->Query("DELETE FROM native_crud WHERE id=?", Value::INTEGER(1)));
			Require(Scalar(*observer->connection, "SELECT count(*) FROM native_crud") == 0, "Native writer DELETE failed");
		});
		test("writer_rollback_discards_rows_and_schema", [&] {
			writer->connection->BeginTransaction();
			SafeCheck(writer->connection->Query("INSERT INTO native_crud VALUES(2,88,'pending')"));
			SafeCheck(writer->connection->Query("CREATE TABLE native_rolled_back(id INTEGER)"));
			writer->connection->Rollback();
			Require(Scalar(*writer->connection, "SELECT count(*) FROM native_crud") == 0 &&
			        Scalar(*writer->connection, "SELECT count(*) FROM information_schema.tables WHERE table_name='native_rolled_back'") == 0,
			        "Native writer rollback retained rows or schema");
		});
		test("writer_disconnect_rolls_back_pending_write", [&] {
			{
				auto abandoned = fixture.Login("writer");
				abandoned->connection->BeginTransaction();
				SafeCheck(abandoned->connection->Query("INSERT INTO native_crud VALUES(3,77,'disconnect')"));
			}
			auto observer = fixture.Login();
			Require(Scalar(*observer->connection, "SELECT count(*) FROM native_crud") == 0, "Native disconnect committed pending work");
			SafeCheck(writer->connection->Query("INSERT INTO native_crud VALUES(3,9,'reused')"));
			SafeCheck(writer->connection->Query("DELETE FROM native_crud WHERE id=3"));
		});
		test("writer_commit_preserves_reader_snapshot_until_transaction_end", [&] {
			SafeCheck(writer->connection->Query("INSERT INTO native_crud VALUES(4,4,'snapshot')"));
			auto reader = fixture.Login();
			reader->connection->BeginTransaction();
			Require(Scalar(*reader->connection, "SELECT stock FROM native_crud WHERE id=4") == 4, "Initial reader snapshot changed");
			SafeCheck(writer->connection->Query("UPDATE native_crud SET stock=5 WHERE id=4"));
			Require(Scalar(*reader->connection, "SELECT stock FROM native_crud WHERE id=4") == 4, "Reader snapshot saw concurrent commit");
			reader->connection->Commit();
			Require(Scalar(*reader->connection, "SELECT stock FROM native_crud WHERE id=4") == 5, "Fresh native transaction missed commit");
			SafeCheck(writer->connection->Query("DELETE FROM native_crud WHERE id=4"));
		});
		test("native_same_username_credentials_are_scoped_per_database", [&] {
			auto primary = fixture.Login("reader");
			auto reporting = fixture.Login("reader", "reporting");
			Require(Scalar(*primary->connection, "SELECT stock FROM fixture_rows") == 10 &&
			        Scalar(*reporting->connection, "SELECT stock FROM fixture_rows") == 20, "Native endpoint selected another engine");
			Require(RejectCredential(fixture.Endpoint("reporting"), fixture.Credential("reader")) &&
			        RejectCredential(fixture.Endpoint(), fixture.Credential("reader", "reporting")), "Native credential crossed a database section");
		});
		test("native_database_scoped_username_is_not_global", [&] {
			auto primary = fixture.Login("primary_only");
			auto reporting = fixture.Login("reporting_only", "reporting");
			RequireHealthy(*primary); RequireHealthy(*reporting);
			Require(RejectCredential(fixture.Endpoint("reporting"), fixture.Credential("primary_only", "reporting")) &&
			        RejectCredential(fixture.Endpoint(), fixture.Credential("reporting_only")), "Native username grant crossed databases");
		});
		test("native_same_username_permissions_are_scoped_per_database", [&] {
			auto primary = fixture.Login("mixed");
			auto reporting = fixture.Login("mixed", "reporting");
			Denied(*primary->connection, "CREATE TABLE mixed_forbidden(id INTEGER)");
			SafeCheck(reporting->connection->Query("CREATE TABLE mixed_allowed(id INTEGER)"));
			SafeCheck(reporting->connection->Query("INSERT INTO mixed_allowed VALUES(2)"));
			Require(Scalar(*reporting->connection, "SELECT sum(id) FROM mixed_allowed") == 2, "Per-database native writer grant was lost");
			SafeCheck(reporting->connection->Query("DROP TABLE mixed_allowed"));
		});
		test("protected_native_shared_owner_token_is_denied", [&] {
			Require(RejectCredential(fixture.Endpoint(), fixture.server->token()) &&
			        RejectCredential(fixture.Endpoint("reporting"), fixture.server->token()), "Shared token bypassed protected native accounts");
		});
		test("native_unknown_account_and_wrong_password_are_denied", [&] {
			Require(RejectCredential(fixture.Endpoint(), "unknown:" + fixture.Password()) &&
			        RejectCredential(fixture.Endpoint(), "reader:" + RandomPassword()), "Native authentication accepted an invalid account");
		});
		test("native_catalog_metadata_lists_only_selected_database", [&] {
			for (const auto &name : {std::string("duckonwire"), std::string("reporting")}) {
				auto reader = fixture.Login("reader", name);
				const auto other = name == "reporting" ? "primary_marker" : "reporting_marker";
				Require(Scalar(*reader->connection, "SELECT count(*) FROM information_schema.tables WHERE table_name='" + std::string(other) + "'") == 0 &&
				        Scalar(*reader->connection, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" + std::string(other) + "'") == 0 &&
				        Scalar(*reader->connection, "SELECT count(*) FROM duckdb_databases() WHERE NOT internal") == 1,
				        "Native metadata exposed another published engine");
			}
		});
		test("native_cross_catalog_read_and_write_are_denied", [&] {
			auto reporting = fixture.Login("writer", "reporting");
			for (const auto &sql : {std::string("SELECT * FROM primary.main.fixture_rows"),
			        std::string("EXPLAIN ANALYZE SELECT * FROM primary.main.fixture_rows"),
			        std::string("CREATE TABLE stolen AS SELECT * FROM primary.main.fixture_rows")}) {
				auto result = reporting->connection->Query(sql);
				Require(result && result->HasError(), "Native SQL accessed another published engine");
			}
			Require(Scalar(*reporting->connection, "SELECT count(*) FROM information_schema.tables WHERE table_name='stolen'") == 0,
			        "Rejected native cross-catalog SQL created a table");
		});
		test("native_concurrent_database_readers_and_writers_keep_routes", [&] {
			std::vector<std::future<bool>> workers;
			for (unsigned worker = 0; worker < 4; ++worker) {
				workers.push_back(std::async(std::launch::async, [&, worker] {
					try {
						const std::string name = worker % 2 ? "reporting" : "duckonwire";
						auto client = fixture.Login(worker < 2 ? "writer" : "reader", name);
						for (unsigned iteration = 0; iteration < 5; ++iteration) {
							client->connection->BeginTransaction();
							Require(Scalar(*client->connection, "SELECT stock FROM fixture_rows") == (name == "reporting" ? 20 : 10),
							        "Concurrent native session switched engines");
							if (worker < 2) SafeCheck(client->connection->Query("INSERT INTO concurrent_rows VALUES(?,?)",
							    Value::INTEGER(iteration), Value::INTEGER(name == "reporting" ? 20 : 10)));
							client->connection->Commit();
						}
						return true;
					} catch (...) { return false; }
				}));
			}
			for (auto &worker : workers) Require(worker.get(), "Concurrent protected native worker failed");
			for (const auto &name : {std::string("duckonwire"), std::string("reporting")}) {
				auto observer = fixture.Login("reader", name);
				Require(Scalar(*observer->connection, "SELECT count(*) FROM concurrent_rows") == 5 &&
				        Scalar(*observer->connection, "SELECT sum(stock) FROM concurrent_rows") == (name == "reporting" ? 100 : 50),
				        "Concurrent native writes crossed database routes");
			}
		});
		test("protected_native_owned_server_graceful_shutdown", [&] {
			writer.reset(); fixture.Stop();
		});

		phase = "native_safe_stored_definitions_fixture_setup";
		Fixture safe_definitions(parent, executable, {}, [](Connection &owner, bool) {
			SafeCheck(owner.Query("CREATE MACRO safe_scalar(x) AS x + 1"));
			SafeCheck(owner.Query("CREATE MACRO safe_table(n) AS TABLE SELECT i FROM range(n) t(i)"));
			SafeCheck(owner.Query("CREATE MACRO safe_nested(x) AS safe_scalar(safe_scalar(x))"));
			SafeCheck(owner.Query("CREATE MACRO safe_default(x := 40) AS x + 2"));
			SafeCheck(owner.Query("CREATE VIEW safe_view AS SELECT safe_nested(stock) AS amount FROM fixture_rows"));
			SafeCheck(owner.Query("CREATE TABLE safe_defaults(id INTEGER DEFAULT 42, label VARCHAR DEFAULT 'quack_connection_id() is an ordinary string')"));
		});
		test("protected_native_preseeded_safe_scalar_macro_is_usable", [&] {
			auto reader = safe_definitions.Login();
			Require(Scalar(*reader->connection, "SELECT safe_scalar(41)") == 42, "Safe persisted scalar macro was rejected");
		});
		test("protected_native_preseeded_safe_table_macro_is_usable", [&] {
			auto reader = safe_definitions.Login();
			Require(Scalar(*reader->connection, "SELECT sum(i) FROM safe_table(10)") == 45, "Safe persisted table macro was rejected");
		});
		test("protected_native_preseeded_safe_nested_macros_and_view_are_usable", [&] {
			for (const auto &name : {std::string("duckonwire"), std::string("reporting")}) {
				auto reader = safe_definitions.Login("reader", name);
				Require(Scalar(*reader->connection, "SELECT safe_nested(40)") == 42 &&
				        Scalar(*reader->connection, "SELECT amount FROM safe_view") == (name == "reporting" ? 22 : 12),
				        "Safe persisted definition chain failed or switched databases");
			}
		});
		test("protected_native_preseeded_safe_macro_default_is_usable", [&] {
			auto reader = safe_definitions.Login();
			Require(Scalar(*reader->connection, "SELECT safe_default()") == 42, "Safe persisted macro default was rejected");
		});
		test("protected_native_preseeded_safe_column_defaults_are_usable", [&] {
			auto owner = safe_definitions.Login("writer");
			SafeCheck(owner->connection->Query("INSERT INTO safe_defaults DEFAULT VALUES"));
			auto reader = safe_definitions.Login();
			Require(Scalar(*reader->connection, "SELECT count(*) FROM safe_defaults WHERE id=42 AND label='quack_connection_id() is an ordinary string'") == 1,
			        "Safe stored column defaults changed or their string literal was rejected");
		});
		test("native_safe_stored_definitions_owned_fixture_graceful_shutdown", [&] { safe_definitions.Stop(); });

		auto unsafe_definition = [&](const std::string &name, const std::function<void(Connection &, const std::string &)> &seed) {
			test(name, [&] {
				const auto payload = "owned-private-definition-" + RandomPassword();
				Fixture unsafe(parent, executable, {}, [&](Connection &owner, bool reporting) {
					if (reporting) return;
					SafeCheck(owner.Query("LOAD quack"));
					seed(owner, payload);
				}, false);
				unsafe.RequireStartupRejected(payload);
			});
		};
		unsafe_definition("protected_native_preseeded_control_view_prevents_startup", [](Connection &owner, const std::string &payload) {
			SafeCheck(owner.Query("CREATE VIEW hidden_view AS SELECT quack_connection_id('unopened_fixture_catalog') AS session_id, " +
			    test_support::Literal(payload) + " AS private_payload"));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_views() WHERE view_name='hidden_view' AND NOT internal") == 1,
			        "Unsafe view fixture was not persisted");
		});
		unsafe_definition("protected_native_preseeded_control_scalar_macro_prevents_startup", [](Connection &owner, const std::string &payload) {
			SafeCheck(owner.Query("CREATE MACRO hidden_scalar() AS quack_connection_id('unopened_fixture_catalog') || " + test_support::Literal(payload)));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_functions() WHERE function_name='hidden_scalar' AND function_type='macro'") == 1,
			        "Unsafe scalar macro fixture was not persisted");
		});
		unsafe_definition("protected_native_preseeded_control_table_macro_prevents_startup", [](Connection &owner, const std::string &payload) {
			SafeCheck(owner.Query("CREATE MACRO hidden_table() AS TABLE SELECT quack_connection_id('unopened_fixture_catalog') AS session_id, " +
			    test_support::Literal(payload) + " AS private_payload"));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_functions() WHERE function_name='hidden_table' AND function_type='table_macro'") == 1,
			        "Unsafe table macro fixture was not persisted");
		});
		unsafe_definition("protected_native_preseeded_nested_control_macros_prevent_startup", [](Connection &owner, const std::string &payload) {
			SafeCheck(owner.Query("CREATE MACRO hidden_leaf() AS quack_connection_id('unopened_fixture_catalog') || " + test_support::Literal(payload)));
			SafeCheck(owner.Query("CREATE MACRO hidden_parent() AS hidden_leaf()"));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_functions() WHERE function_name IN ('hidden_leaf','hidden_parent') AND function_type='macro'") == 2,
			        "Unsafe nested macro fixture was not persisted");
		});
		unsafe_definition("protected_native_preseeded_control_column_default_prevents_startup", [](Connection &owner, const std::string &payload) {
			SafeCheck(owner.Query("CREATE TABLE hidden_default(id VARCHAR DEFAULT quack_connection_id('unopened_fixture_catalog') || " +
			    test_support::Literal(payload) + ")"));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_columns() WHERE table_name='hidden_default' AND column_default IS NOT NULL") == 1,
			        "Unsafe column default fixture was not persisted");
		});
		unsafe_definition("protected_native_preseeded_file_macro_prevents_startup", [](Connection &owner, const std::string &payload) {
			auto catalog = owner.Query("SELECT path FROM duckdb_databases() WHERE NOT internal");
			SafeCheck(catalog);
			Require(catalog->RowCount() == 1, "Unsafe file fixture database path is ambiguous");
			const auto input = fs::path(catalog->GetValue(0, 0).GetValue<std::string>()).parent_path() / "owned-private-input.json";
			{
				std::ofstream file(input);
				file << "{\"private_payload\":" << duckonwire::JsonString(payload) << "}\n";
				file.close();
				Require(bool(file) && chmod(input.c_str(), 0600) == 0, "Cannot write owned private macro input");
			}
			SafeCheck(owner.Query("CREATE MACRO hidden_file() AS TABLE SELECT * FROM read_json_auto(" + test_support::Literal(input.string()) + ")"));
			Require(Scalar(owner, "SELECT count(*) FROM duckdb_functions() WHERE function_name='hidden_file' AND function_type='table_macro'") == 1,
			        "Unsafe file macro fixture was not persisted");
		});

		phase = "native_bruteforce_fixture_setup";
		Fixture brute_force(parent, executable, {"--auth-max-failures", "2"});
		auto established_primary = brute_force.Login();
		auto established_reporting = brute_force.Login("reader", "reporting");
		RequireHealthy(*established_primary); RequireHealthy(*established_reporting);
		Require(PostgreSQLTLSAccepted(brute_force.server->pg_port()), "Cross-protocol fixture did not offer PostgreSQL TLS before ban");
		test("native_invalid_passwords_trigger_peer_ban", [&] {
			Require(RejectCredential(brute_force.Endpoint(), "reader:" + RandomPassword()) &&
			        RejectCredential(brute_force.Endpoint(), "reader:" + RandomPassword()), "Invalid native passwords were accepted");
			Require(RejectCredential(brute_force.Endpoint("reporting"), brute_force.Credential("reader", "reporting")),
			        "Valid native credential evaded same-peer brute-force ban");
		});
		test("native_peer_ban_rejects_reused_sessions_in_all_databases", [&] {
			Require(RequestRejected(*established_primary) && RequestRejected(*established_reporting),
			        "An existing native session bypassed shared peer ban");
		});
		test("native_bruteforce_ban_also_rejects_postgresql_tls_startup", [&] {
			Require(!PostgreSQLTLSAccepted(brute_force.server->pg_port()), "PostgreSQL transport bypassed native peer ban");
		});
		test("native_bruteforce_ban_expiry_allows_valid_authentication", [&] {
			WaitForBanExpiry();
			auto recovered = brute_force.Login("reader", "reporting");
			RequireHealthy(*recovered);
		});
		test("native_bruteforce_owned_fixture_graceful_shutdown", [&] {
			established_primary.reset(); established_reporting.reset(); brute_force.Stop();
		});

		phase = "native_request_cap_fixture_setup";
		Fixture request_cap(parent, executable, {"--max-requests-per-minute", "128"});
		auto flooding = request_cap.Login();
		auto request_peer = request_cap.Login("reader", "reporting");
		RequireHealthy(*flooding); RequireHealthy(*request_peer);
		test("native_excess_rpc_requests_trigger_peer_ban", [&] {
			bool rejected = false;
			for (unsigned request = 0; request < 512 && !rejected; ++request) rejected = RequestRejected(*flooding);
			Require(rejected, "Native RPC request quota did not stop bounded flood");
			Require(RequestRejected(*request_peer), "Another native database bypassed shared request quota");
		});
		test("native_request_cap_ban_expiry_resets_counters", [&] {
			WaitForBanExpiry();
			auto recovered = request_cap.Login("reader", "reporting");
			RequireHealthy(*recovered);
		});
		test("native_request_cap_owned_fixture_graceful_shutdown", [&] {
			flooding.reset(); request_peer.reset(); request_cap.Stop();
		});

		phase = "native_connection_cap_fixture_setup";
		Fixture connection_cap(parent, executable, {"--max-connections-per-minute", "12"});
		auto connection_peer = connection_cap.Login("reader", "reporting");
		RequireHealthy(*connection_peer);
		test("native_excess_logical_connections_trigger_shared_peer_ban", [&] {
			bool rejected = false;
			for (unsigned attempt = 0; attempt < 24 && !rejected; ++attempt)
				rejected = RejectCredential(connection_cap.Endpoint(), connection_cap.Credential("reader"));
			Require(rejected && RequestRejected(*connection_peer), "Native logical connection quota did not ban shared peer");
		});
		test("native_connection_cap_ban_expiry_allows_login", [&] {
			WaitForBanExpiry();
			auto recovered = connection_cap.Login();
			RequireHealthy(*recovered);
		});
		test("native_connection_cap_owned_fixture_graceful_shutdown", [&] {
			connection_peer.reset(); connection_cap.Stop();
		});
	} catch (...) {
		checks.push_back({phase, false});
	}
	bool passed = !checks.empty();
	for (const auto &check : checks) passed = passed && check.passed;
	std::cout << "{\"suite\":\"native_access\",\"passed\":" << (passed ? "true" : "false")
	          << ",\"native_used\":true,\"source_id\":" << duckonwire::JsonString(DuckDB::SourceID()) << ",\"checks\":[";
	for (std::size_t i = 0; i < checks.size(); ++i) {
		if (i) std::cout << ',';
		std::cout << "{\"name\":" << duckonwire::JsonString(checks[i].name)
		          << ",\"passed\":" << (checks[i].passed ? "true" : "false");
		if (!checks[i].passed) std::cout << ",\"error\":\"Native security acceptance failed; credentials are not logged\"";
		if (!checks[i].reason.empty()) std::cout << ",\"reason\":" << duckonwire::JsonString(checks[i].reason);
		std::cout << '}';
	}
	std::cout << "]}\n";
	return passed ? 0 : 1;
}
