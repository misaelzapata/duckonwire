#include "duckdb.hpp"
#include "duckonwire/json.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace duckdb;

namespace {
// Connection options belong to SQL's ATTACH/CONNECT grammar. Data values below
// use native parameters, including the strings and DECIMAL amounts.
std::string Literal(const std::string &value) {
	std::string result = "'";
	for (char byte : value) {
		if (byte == '\0') throw std::runtime_error("Invalid connection setting");
		result += byte;
		if (byte == '\'') result += byte;
	}
	return result + "'";
}
template <class T> void Check(const unique_ptr<T> &result) {
	if (!result || result->HasError()) throw std::runtime_error("Native statement failed");
}
void SQL(Connection &connection, const std::string &sql) { Check(connection.Query(sql)); }
int64_t Scalar(Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql); Check(result);
	if (result->RowCount() != 1) throw std::runtime_error("Expected one result row");
	return result->GetValue<int64_t>(0, 0);
}
void Rollback(Connection &connection) noexcept {
	try { if (!connection.IsAutoCommit()) connection.Rollback(); } catch (...) {}
}
std::string UniqueTable() {
	std::random_device random;
	std::ostringstream name;
	name << "duckonwire_attach_" << std::hex
	     << std::chrono::steady_clock::now().time_since_epoch().count()
	     << random() << random();
	return name.str(); // Only identifier-safe generated characters; no input SQL.
}
void Emit(bool passed, const std::vector<std::string> &checks, const std::string &phase,
          const std::string &error = "") {
	std::cout << "{\"passed\":" << (passed ? "true" : "false")
	          << ",\"client\":\"native C++ duckdb::Connection\",\"native_used\":true"
	          << ",\"library_version\":" << duckonwire::JsonString(DuckDB::LibraryVersion())
	          << ",\"source_id\":" << duckonwire::JsonString(DuckDB::SourceID())
	          << ",\"scope\":\"ATTACH reads and local/remote join; CONNECT alias CRUD and independent transactions\""
	          << ",\"checks\":[";
	for (size_t i = 0; i < checks.size(); ++i) {
		if (i) std::cout << ',';
		std::cout << "{\"name\":" << duckonwire::JsonString(checks[i]) << ",\"passed\":true}";
	}
	std::cout << "],\"phase\":" << duckonwire::JsonString(phase);
	if (!error.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(error);
	std::cout << "}\n";
}
} // namespace

int main() {
	std::vector<std::string> checks;
	std::string phase = "configuration";
	const char *token_env = std::getenv("DUCKONWIRE_TOKEN");
	if (!token_env || !*token_env) {
		Emit(false, checks, phase, "DUCKONWIRE_TOKEN is required");
		return 2;
	}
	try {
		const std::string token = token_env;
		const char *endpoint_env = std::getenv("DUCKONWIRE_ENDPOINT");
		const std::string endpoint = endpoint_env && *endpoint_env ? endpoint_env : "quack:127.0.0.1:9495";
		DBConfig config;
		config.SetOptionByName("threads", Value::BIGINT(2));
		DuckDB database(nullptr, &config); // The owner holds the persistent file.
		Connection setup(database), client(database);
		const auto table = UniqueTable();
		bool fixture_owned = false;
		auto require = [&](const std::string &name, bool passed) {
			phase = name;
			if (!passed) throw std::runtime_error("Unexpected native result");
			checks.push_back(name);
		};
		try {
			phase = "create_owned_remote_fixture";
			SQL(setup, "CONNECT " + Literal(endpoint) + " (TOKEN " + Literal(token) + ")");
			SQL(setup, "CREATE TABLE " + table + "(id INTEGER PRIMARY KEY, name VARCHAR, price DECIMAL(18,2), stock INTEGER)");
			fixture_owned = true; // Cleanup only follows our successful CREATE.
			auto insert = setup.Prepare("INSERT INTO " + table + " VALUES(?,?,?,?)"); Check(insert);
			Check(insert->Execute(Value::INTEGER(1), Value("Coffee"), Value::DECIMAL(int64_t(1999), 18, 2), Value::INTEGER(3)));
			Check(insert->Execute(Value::INTEGER(2), Value("Rice"), Value::DECIMAL(int64_t(850), 18, 2), Value::INTEGER(10)));
			insert.reset();

			phase = "attach_alias_catalog";
			SQL(client, "ATTACH " + Literal(endpoint) + " AS remote (TOKEN " + Literal(token) + ")");
			require(phase, Scalar(client, "SELECT count(*) FROM remote.main." + table) == 2);
			// ATTACH's current query pushdown does not transport placeholders.
			// Read directly here; use CONNECT for parameterized remote statements.
			phase = "attach_remote_read";
			auto row = client.Query("SELECT name,price,stock FROM remote.main." + table + " ORDER BY id LIMIT 1"); Check(row);
			auto chunk = row->Fetch();
			require(phase, chunk && chunk->size() == 1 && chunk->GetValue(0, 0).GetValue<std::string>() == "Coffee" &&
			        row->GetTypes()[1] == LogicalType::DECIMAL(18, 2) && chunk->GetValue(1, 0).ToString() == "19.99" && chunk->GetValue(2, 0).GetValue<int64_t>() == 3);
			row.reset();
			phase = "attach_join_local_and_remote";
			SQL(client, "CREATE TEMP TABLE local_labels(id INTEGER, label VARCHAR)");
			Check(client.Query("INSERT INTO local_labels VALUES(?,?)", Value::INTEGER(1), Value("local tag")));
			auto joined = client.Query("SELECT r.name,l.label FROM remote.main." + table + " r JOIN temp.main.local_labels l ON l.id=r.id ORDER BY r.id LIMIT 10"); Check(joined);
			require(phase, joined->RowCount() == 1 && joined->GetValue(0, 0).GetValue<std::string>() == "Coffee" && joined->GetValue(1, 0).GetValue<std::string>() == "local tag");
			joined.reset();

			// ATTACH-only mutation planning is incomplete. CONNECT routes whole
			// statements, typed parameters and transaction control to the owner.
			phase = "connect_alias_parameterized_crud_commit";
			SQL(client, "CONNECT remote");
			const auto stock_sql = "SELECT stock::BIGINT FROM " + table + " WHERE id=1";
			client.BeginTransaction();
			Check(client.Query("UPDATE " + table + " SET stock=stock+? WHERE id=?", Value::INTEGER(1), Value::INTEGER(1)));
			auto change = client.Prepare("INSERT INTO " + table + " VALUES(?,?,?,?)"); Check(change);
			Check(change->Execute(Value::INTEGER(3), Value("Whole-grain biscuits"), Value::DECIMAL(int64_t(1250), 18, 2), Value::INTEGER(7)));
			change.reset();
			Check(client.Query("DELETE FROM " + table + " WHERE id=?", Value::INTEGER(2)));
			client.Commit();
			require(phase, Scalar(setup, "SELECT count(*) FROM " + table) == 2 && Scalar(setup, stock_sql) == 4);
			phase = "connect_alias_rollback";
			client.BeginTransaction();
			Check(client.Query("UPDATE " + table + " SET stock=? WHERE id=?", Value::INTEGER(99), Value::INTEGER(1)));
			client.Rollback();
			require(phase, Scalar(setup, stock_sql) == 4);
			SQL(client, "DISCONNECT");
			require("disconnect_restores_local_queries", Scalar(client, "SELECT count(*) FROM temp.main.local_labels") == 1);
			SQL(setup, "DROP TABLE " + table); fixture_owned = false;
			auto absent = setup.Query("SELECT count(*) FROM information_schema.tables WHERE table_name=?", Value(table)); Check(absent);
			auto cleanup_row = absent->Fetch();
			require("owned_remote_table_removed", cleanup_row && cleanup_row->size() == 1 && cleanup_row->GetValue(0, 0).GetValue<int64_t>() == 0); absent.reset();
			SQL(client, "DETACH remote");
			require("detach_removes_remote_catalog", Scalar(client, "SELECT count(*) FROM duckdb_databases() WHERE database_name='remote'") == 0);
		} catch (...) {
			Rollback(client); Rollback(setup);
			if (fixture_owned) {
				try { SQL(setup, "DROP TABLE " + table); } catch (...) {}
			}
			throw;
		}
		Emit(true, checks, "complete");
		return 0;
	} catch (...) {
		// Native errors may include ATTACH SQL. Never print connection secrets.
		Emit(false, checks, phase, "Native ATTACH example failed; connection settings are not logged");
		return 1;
	}
}
