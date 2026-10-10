#include "duckdb.hpp"
#include "duckonwire/json.hpp"
#include "native_server.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/wait.h>
#include <thread>

using namespace duckdb;
using test_support::Check;
using test_support::Connect;
using test_support::NativeServer;
using test_support::Require;
using test_support::TempRoot;
namespace fs = std::filesystem;

namespace {
int64_t Scalar(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	Check(result);
	return result->GetValue<int64_t>(0, 0);
}
void SQL(Connection &con, const std::string &sql) { Check(con.Query(sql)); }
int RunFlow(const fs::path &program, const NativeServer &server) {
	std::string endpoint = "DUCKONWIRE_ENDPOINT=" + server.endpoint();
	std::string token = "DUCKONWIRE_TOKEN=" + server.token();
	std::vector<char *> env;
	for (char **item = environ; *item; ++item) {
		if (std::strncmp(*item, "DUCKONWIRE_ENDPOINT=", 20) != 0 &&
		    std::strncmp(*item, "DUCKONWIRE_TOKEN=", 17) != 0) env.push_back(*item);
	}
	env.push_back(endpoint.data()); env.push_back(token.data()); env.push_back(nullptr);
	std::string name = fs::canonical(program).string();
	char *args[] = {name.data(), nullptr};
	pid_t child = -1;
	Require(posix_spawn(&child, name.c_str(), nullptr, nullptr, args, env.data()) == 0, "Cannot run native flow");
	int status = 0;
	while (waitpid(child, &status, 0) < 0) Require(errno == EINTR, "Cannot wait for native flow");
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
} // namespace

int main(int argc, char **argv) {
	fs::path executable, flow, catalog;
	for (int i = 1; i < argc; ++i) {
		std::string option = argv[i];
		if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
		if (option == "--server") executable = argv[++i];
		else if (option == "--flow") flow = argv[++i];
		else if (option == "--catalog") catalog = argv[++i];
		else { std::cerr << "Unknown option\n"; return 2; }
	}
	std::vector<std::string> passed;
	std::string current;
	try {
		Require(!executable.empty() && !flow.empty(), "--server and --flow are required");
		if (catalog.empty()) catalog = fs::canonical(executable).parent_path().parent_path().parent_path() / "examples/demo";
		TempRoot fixture;
		const auto file = fixture.path() / "native.duckdb";
		auto server = std::make_unique<NativeServer>(fixture.path(), file, executable);
		DBConfig config;
		config.SetOptionByName("threads", Value::BIGINT(2));
		DuckDB client(nullptr, &config);
		auto connection = [&]() {
			auto con = std::make_unique<Connection>(client);
			Connect(*con, server->endpoint(), server->token());
			return con;
		};
		auto test = [&](const std::string &name, const std::function<void()> &body) {
			current = name;
			body(); passed.push_back(name);
		};
		test("builtin_extensions_and_private_descriptor", [&] {
			auto con = connection();
			Require(Scalar(*con, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'") == 2, "Extensions are not builtin");
			Require(Scalar(*con, "SELECT NOT current_setting('allow_unsigned_extensions')") == 1, "Unsigned extensions enabled");
			struct stat state {};
			Require(lstat(server->state().c_str(), &state) == 0 && (state.st_mode & 0777) == 0600, "Descriptor permissions");
		});
		test("typed_parameter_crud_no_sql_interpolation", [&] {
			auto con = connection();
			SQL(*con, "CREATE TABLE products_test(id INTEGER PRIMARY KEY,name VARCHAR,stock INTEGER)");
			auto statement = con->Prepare("INSERT INTO products_test VALUES (?, ?, ?)");
			Require(!statement->HasError(), "Prepare failed");
			Check(statement->Execute(Value::INTEGER(1), Value("Cafe'; DROP TABLE products_test; --"), Value::INTEGER(3)));
			Check(con->Query("UPDATE products_test SET stock=? WHERE id=?", Value::INTEGER(4), Value::INTEGER(1)));
			Require(Scalar(*con, "SELECT stock FROM products_test WHERE id=1") == 4, "Typed CRUD mismatch");
		});
		test("named_parameters", [&] {
			auto con = connection();
			auto statement = con->Prepare("SELECT $left::INTEGER + $right::INTEGER");
			Require(!statement->HasError(), "Named prepare failed");
			identifier_map_t<BoundParameterData> values;
			values[Identifier("left")] = BoundParameterData(Value::INTEGER(40));
			values[Identifier("right")] = BoundParameterData(Value::INTEGER(2));
			auto result = statement->Execute(values, false); Check(result);
			Require(result->Fetch()->GetValue(0, 0).GetValue<int64_t>() == 42, "Named value transport");
		});
		test("repeated_prepared_batch_and_commit", [&] {
			auto con = connection();
			SQL(*con, "CREATE TABLE batch_test(id INTEGER PRIMARY KEY,value BIGINT)");
			con->BeginTransaction();
			auto statement = con->Prepare("INSERT INTO batch_test VALUES (?, ?)");
			for (int i = 0; i < 100; ++i) Check(statement->Execute(Value::INTEGER(i), Value::BIGINT(i * 2)));
			con->Commit();
			Require(Scalar(*con, "SELECT sum(value) FROM batch_test") == 9900, "Batch checksum");
		});
		test("complex_typed_parameters_and_null", [&] {
			auto con = connection();
			auto statement = con->Prepare("SELECT ? AS items, ? AS object, ? AS amount, ? AS missing");
			vector<Value> values {Value::LIST(LogicalType::INTEGER, {Value::INTEGER(1), Value()}),
			                     Value::STRUCT({{"name", Value("Cafe")}, {"stock", Value::INTEGER(4)}}),
			                     Value::DECIMAL(int64_t(12345), 18, 2), Value()};
			auto result = statement->Execute(values, false); Check(result);
			auto chunk = result->Fetch();
			for (idx_t i = 0; i < values.size(); ++i) {
				const auto actual = chunk->GetValue(i, 0);
				Require(values[i].IsNull() ? actual.IsNull() :
				        actual.type() == values[i].type() && actual.ToString() == values[i].ToString(),
				        "Complex typed value changed");
			}
		});
		test("native_result_types_and_duplicate_names", [&] {
			auto con = connection();
			auto result = con->Query("SELECT DATE '2026-01-15' AS value, TIMESTAMP '2026-01-15 12:34:56' AS value, 19.99::DECIMAL(18,2), [1,2], {'name':'Cafe'}, NULL::INTEGER");
			Check(result);
			Require(result->GetTypes()[0] == LogicalType::DATE && result->GetTypes()[1] == LogicalType::TIMESTAMP && result->GetTypes()[2] == LogicalType::DECIMAL(18, 2), "Native type metadata");
			Require(result->GetNames()[0].GetIdentifierName() == "value" && result->GetNames()[1].GetIdentifierName() == "value" && result->GetValue(5, 0).IsNull(), "Native names/null");
		});
		test("ddl_visibility_between_sessions", [&] {
			auto writer = connection(), reader = connection();
			SQL(*writer, "CREATE TABLE ddl_test(id INTEGER)");
			Require(Scalar(*reader, "SELECT count(*) FROM ddl_test") == 0, "CREATE invisible");
			SQL(*writer, "ALTER TABLE ddl_test ADD COLUMN label VARCHAR DEFAULT 'new'");
			Check(writer->Query("INSERT INTO ddl_test(id) VALUES (?)", Value::INTEGER(1)));
			auto result = reader->Query("SELECT label FROM ddl_test"); Check(result);
			Require(result->GetValue(0, 0).GetValue<std::string>() == "new", "ALTER invisible");
		});
		test("sql_and_bind_errors_leave_session_usable", [&] {
			auto con = connection();
			Require(con->Query("SELECT nonexistent_column FROM products_test")->HasError(), "Expected binder error");
			auto bad = con->Prepare("SELECT ?::INTEGER");
			Require(bad->Execute(Value("not an integer"))->HasError(), "Expected conversion error");
			Require(Scalar(*con, "SELECT 42") == 42, "Session failed after error");
		});
		test("native_begin_commit_rollback_methods", [&] {
			auto con = connection();
			con->BeginTransaction(); SQL(*con, "UPDATE products_test SET stock=7 WHERE id=1"); con->Rollback();
			Require(Scalar(*con, "SELECT stock FROM products_test WHERE id=1") == 4, "Method rollback failed");
			con->BeginTransaction(); SQL(*con, "UPDATE products_test SET stock=5 WHERE id=1"); con->Commit();
			Require(Scalar(*con, "SELECT stock FROM products_test WHERE id=1") == 5, "Method commit failed");
		});
		test("pending_changes_are_isolated", [&] {
			auto writer = connection(), reader = connection();
			writer->BeginTransaction(); SQL(*writer, "INSERT INTO products_test VALUES(2,'Arroz',9)");
			Require(Scalar(*reader, "SELECT count(*) FROM products_test") == 1, "Dirty read");
			writer->Rollback();
		});
		test("closing_session_rolls_back", [&] {
			{ auto con = connection(); con->BeginTransaction(); SQL(*con, "INSERT INTO products_test VALUES(3,'Pending',9)"); }
			auto reader = connection();
			Require(Scalar(*reader, "SELECT count(*) FROM products_test WHERE id=3") == 0, "Close committed pending data");
		});
		test("reader_snapshot_until_new_transaction", [&] {
			auto writer = connection(), reader = connection();
			reader->BeginTransaction(); Require(Scalar(*reader, "SELECT stock FROM products_test WHERE id=1") == 5, "Initial snapshot");
			SQL(*writer, "UPDATE products_test SET stock=6 WHERE id=1");
			Require(Scalar(*reader, "SELECT stock FROM products_test WHERE id=1") == 5, "Snapshot changed");
			reader->Commit(); reader->BeginTransaction();
			Require(Scalar(*reader, "SELECT stock FROM products_test WHERE id=1") == 6, "New snapshot stale"); reader->Commit();
		});
		test("same_row_conflict_and_rollback_recovery", [&] {
			auto first = connection(), second = connection();
			first->BeginTransaction(); second->BeginTransaction();
			SQL(*first, "UPDATE products_test SET stock=10 WHERE id=1");
			auto conflict = second->Query("UPDATE products_test SET stock=11 WHERE id=1");
			Require(conflict->HasError() && conflict->GetError().find("Conflict") != std::string::npos, "Native conflict missing");
			second->Rollback(); first->Rollback();
			Require(Scalar(*second, "SELECT stock FROM products_test WHERE id=1") == 6, "Conflict recovery");
		});
		test("streaming_result_pages", [&] {
			auto con = connection(); auto result = con->SendQuery("SELECT i FROM range(10000) r(i) ORDER BY i"); Check(result);
			int64_t count = 0, sum = 0;
			while (auto chunk = result->Fetch()) { if (!chunk->size()) break; for (idx_t i = 0; i < chunk->size(); ++i) { sum += chunk->GetValue(0, i).GetValue<int64_t>(); ++count; } }
			Require(count == 10000 && sum == 49995000, "Result pagination lost rows");
		});
		test("commit_without_active_transaction_errors", [&] {
			auto con = connection(); Require(con->Query("COMMIT")->HasError(), "SQL COMMIT unexpectedly succeeded");
			Require(Scalar(*con, "SELECT 1") == 1, "Session unusable after inactive COMMIT");
		});
		test("remote_query_interrupt_and_recovery", [&] {
			auto con = connection(); std::atomic<bool> started {false}, failed {false};
			std::thread worker([&] { started = true; auto result = con->Query("SELECT sum(a.i*b.i) FROM range(100000000) a(i), range(100000000) b(i)"); failed = result->HasError(); });
			while (!started) std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(100)); con->Interrupt(); worker.join();
			Require(failed, "Interrupt did not cancel query"); Require(Scalar(*con, "SELECT 42") == 42, "Interrupt recovery failed");
		});
		test("reader_progress_while_writer_pending", [&] {
			auto writer = connection(), reader = connection(); writer->BeginTransaction();
			SQL(*writer, "INSERT INTO products_test VALUES(4,'Pending',1)");
			auto begin = std::chrono::steady_clock::now();
			Require(Scalar(*reader, "SELECT count(*) FROM products_test") == 1, "Dirty read during write");
			Require(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() < 2, "Reader blocked by writer"); writer->Rollback();
		});
		test("graceful_restart_persistence", [&] {
			server->stop(); Require(!server->forced_stop() && !fs::exists(server->state()), "Graceful stop failed");
			server = std::make_unique<NativeServer>(fixture.path(), file, executable);
			auto con = connection(); Require(Scalar(*con, "SELECT stock FROM products_test WHERE id=1") == 6, "Restart lost commit");
		});
		test("sigkill_recovers_commits_and_discards_pending", [&] {
			{ auto con = connection(); SQL(*con, "INSERT INTO products_test VALUES(5,'Durable',1)"); con->BeginTransaction(); SQL(*con, "INSERT INTO products_test VALUES(6,'Pending',1)"); Require(kill(server->pid(), SIGKILL) == 0, "Cannot crash fixture"); server->stop(); }
			server = std::make_unique<NativeServer>(fixture.path(), file, executable);
			auto con = connection(); Require(Scalar(*con, "SELECT count(*) FROM products_test WHERE id=5") == 1 && Scalar(*con, "SELECT count(*) FROM products_test WHERE id=6") == 0, "WAL recovery mismatch");
		});
		test("real_cpp_transaction_flow", [&] { Require(RunFlow(flow, *server) == 0, "Native transaction-flow executable failed"); });
		test("native_demo_initialization_and_existing_data_protection", [&] {
			TempRoot demo; fs::create_directories(demo.path() / "examples/demo");
			for (const auto *name : {"products.csv", "prices.csv"}) fs::copy_file(catalog / name, demo.path() / "examples/demo" / name);
			const auto db = demo.path() / "demo.duckdb";
			{ NativeServer initialized(demo.path(), db, executable, 0, true); Connection con(client); Connect(con, initialized.endpoint(), initialized.token());
			  Require(Scalar(con, "SELECT count(*) FROM products") == 46462 && Scalar(con, "SELECT count(*) FROM prices") == 46462, "Native demo counts");
			  Require(Scalar(con, "SELECT count(DISTINCT brand) FROM products") == 178, "Demo brands");
			  Require(Scalar(con, "SELECT count(*) FROM products WHERE category='' OR size='' OR unit=''") == 32909 &&
			          Scalar(con, "SELECT count(*) FROM products WHERE category IS NULL OR size IS NULL OR unit IS NULL") == 0,
			          "CSV empty fields were not preserved as strings");
			  Require(Scalar(con, "SELECT current_setting('threads')") == 2, "Default engine threads changed"); }
			bool rejected = false; try { NativeServer duplicate(demo.path(), db, executable, 0, true); } catch (const std::exception &error) { rejected = std::string(error.what()).find("requires an empty database") != std::string::npos; }
			Require(rejected, "Demo initialization overwrote existing database");
			{ NativeServer reopened(demo.path(), db, executable); Connection con(client); Connect(con, reopened.endpoint(), reopened.token()); Require(Scalar(con, "SELECT count(*) FROM products") == 46462, "Existing demo changed"); }

			// History uses native bound generation, not a million-row CSV. Compute independent exact checksums.
			fs::remove(demo.path() / "examples/demo/prices.csv");
			constexpr int64_t rows = 1000000;
			int64_t product_sum = 0, cents_sum = 0, days_sum = 0;
			for (int64_t id = 1; id <= rows; ++id) {
				product_sum += 1 + (id - 1) % 46462;
				cents_sum += 50 + (id * 48271 + 12345) % 24951;
				days_sum += ((id * 1103515245 + 12345) % 2147483648) % 30;
			}
			auto verify_history = [&](Connection &con) {
				Require(Scalar(con, "SELECT count(*) FROM prices") == rows &&
				        Scalar(con, "SELECT count(DISTINCT id) FROM prices") == rows &&
				        Scalar(con, "SELECT min(id) FROM prices") == 1 && Scalar(con, "SELECT max(id) FROM prices") == rows &&
				        Scalar(con, "SELECT sum(id)::BIGINT FROM prices") == rows * (rows + 1) / 2 &&
				        Scalar(con, "SELECT sum(product_id)::BIGINT FROM prices") == product_sum &&
				        Scalar(con, "SELECT sum(price*100)::BIGINT FROM prices") == cents_sum &&
				        Scalar(con, "SELECT sum(day-DATE '2026-01-01')::BIGINT FROM prices") == days_sum,
				        "Synthetic history count, domain or checksum mismatch");
				Require(Scalar(con, "SELECT count(DISTINCT product_id) FROM prices") == 46462 &&
				        Scalar(con, "SELECT count(DISTINCT day) FROM prices") == 30 &&
				        Scalar(con, "SELECT count(*) FROM prices WHERE price NOT BETWEEN 0.50 AND 250.00 OR day NOT BETWEEN DATE '2026-01-01' AND DATE '2026-01-30'") == 0 &&
				        Scalar(con, "SELECT count(*) FROM prices r LEFT JOIN products p ON p.id=r.product_id WHERE p.id IS NULL") == 0,
				        "Synthetic history countries, days, prices or references differ");
			};
			const auto history_db = demo.path() / "history.duckdb";
			{ NativeServer initialized(demo.path(), history_db, executable, 0, true, {"--demo-prices", "1000000", "--threads", "1"});
			  Connection con(client); Connect(con, initialized.endpoint(), initialized.token()); verify_history(con);
			  Require(Scalar(con, "SELECT current_setting('threads')") == 1, "Configured engine threads ignored");
			  for (const auto *invalid : {
			      "INSERT INTO products VALUES(46463,'','Brand','','','','AR')",
			      "INSERT INTO products VALUES(46463,'Name','','','','','AR')",
			      "INSERT INTO products VALUES(46463,NULL,'Brand','','','','AR')",
			      "INSERT INTO products VALUES(46463,'Name','Brand',NULL,'','','AR')",
			      "INSERT INTO products VALUES(46463,'Name','Brand','',NULL,'','AR')",
			      "INSERT INTO products VALUES(46463,'Name','Brand','','',NULL,'AR')",
			      "INSERT INTO products VALUES(46463,'Name','Brand','','','','ZZ')",
			      "INSERT INTO prices VALUES(1000001,0,1.00,DATE '2026-01-01')",
			      "INSERT INTO prices VALUES(1000001,NULL,1.00,DATE '2026-01-01')",
			      "INSERT INTO prices VALUES(1000001,1,NULL,DATE '2026-01-01')",
			      "INSERT INTO prices VALUES(1000001,1,0.49,DATE '2026-01-01')",
			      "INSERT INTO prices VALUES(1000001,1,250.01,DATE '2026-01-01')",
			      "INSERT INTO prices VALUES(1000001,1,1.00,NULL)",
			      "INSERT INTO prices VALUES(1,1,1.00,DATE '2026-01-01')",
			      "DELETE FROM products WHERE id=1"}) {
				con.BeginTransaction(); auto result = con.Query(invalid);
				const bool denied = result->HasError() && result->GetErrorObject().Type() == ExceptionType::CONSTRAINT;
				con.Rollback(); Require(denied, "Initialized demo did not retain a schema constraint");
			  }
			  con.BeginTransaction(); SQL(con, "INSERT INTO prices VALUES(1000001,1,1.00,DATE '2026-01-01'),(1000002,1,2.00,DATE '2026-01-02')");
			  Require(Scalar(con, "SELECT count(*) FROM prices WHERE id>1000000") == 2, "Multiple prices for one product rejected");
			  con.Rollback(); verify_history(con); }
			{ NativeServer reopened(demo.path(), history_db, executable, 0, false, {"--threads", "64"});
			  Connection con(client); Connect(con, reopened.endpoint(), reopened.token()); verify_history(con);
			  Require(Scalar(con, "SELECT current_setting('threads')") == 64, "Maximum engine threads rejected"); }

			int invalid_number = 0;
			for (const auto &arguments : std::vector<std::vector<std::string>>{
			    {"--demo-prices", "1000000"}, {"--init-demo", "--demo-prices", "46461"},
			    {"--init-demo", "--demo-prices", "10000001"}, {"--init-demo", "--demo-prices", "1000000junk"},
			    {"--threads", "0"}, {"--threads", "65"}, {"--threads", "2junk"}}) {
				const auto invalid_db = demo.path() / ("invalid-" + std::to_string(++invalid_number) + ".duckdb");
				bool denied = false; try { NativeServer bad(demo.path(), invalid_db, executable, 0, false, arguments); }
				catch (const std::exception &) { denied = true; }
				Require(denied && !fs::exists(invalid_db), "Invalid demo/thread arguments created a database");
			}

			// A bad CSV must roll back both table creation and data loading.
			TempRoot bad; fs::create_directories(bad.path() / "examples/demo");
			std::ifstream csv(catalog / "products.csv"); std::string contents((std::istreambuf_iterator<char>(csv)), {});
			const auto position = contents.find(",AR\n"); Require(position != std::string::npos, "Country fixture unavailable");
			contents.replace(position, 4, ",ZZ\n");
			{ std::ofstream output(bad.path() / "examples/demo/products.csv"); output << contents; }
			const auto bad_db = bad.path() / "rollback.duckdb";
			bool failed = false; try { NativeServer invalid(bad.path(), bad_db, executable, 0, true, {"--demo-prices", "1000000"}); }
			catch (const std::exception &) { failed = true; }
			Require(failed, "Invalid CSV initialized successfully");
			DuckDB rolled_back(bad_db.c_str()); Connection local(rolled_back);
			Require(Scalar(local, "SELECT count(*) FROM duckdb_tables() WHERE NOT internal AND NOT temporary") == 0,
			        "Failed initialization committed partial schema or data");
		});
		server->stop(); Require(!server->forced_stop(), "Final server shutdown timed out");
		Require(passed.size() == 21, "Not all required native checks ran");
		std::cout << "{\"passed\":true,\"native_checks\":" << passed.size() << ",\"checks\":[";
		for (size_t i = 0; i < passed.size(); ++i) std::cout << (i ? "," : "") << duckonwire::JsonString(passed[i]);
		std::cout << "]}\n"; return 0;
	} catch (const std::exception &error) {
		std::cerr << "Native check failed: " << current << ": " << error.what() << '\n';
		std::cout << "{\"passed\":false,\"native_checks\":" << passed.size() << ",\"failed_check\":" << duckonwire::JsonString(current) << "}\n";
		return 1;
	}
}
