#include "duckdb.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace duckdb;

namespace {
struct Observation {
	string name;
	string expected;
	string actual;
	bool passed;
};

static string Quote(const string &value) {
	string result = "'";
	for (char ch : value) {
		result += ch == '\'' ? "''" : string(1, ch);
	}
	return result + "'";
}

static string JsonString(const string &value) {
	std::ostringstream out;
	out << '"';
	for (unsigned char ch : value) {
		switch (ch) {
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (ch < 32) {
				out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(ch) << std::dec;
			} else {
				out << char(ch);
			}
		}
	}
	out << '"';
	return out.str();
}

template <class T> static void Check(const unique_ptr<T> &result) {
	if (!result) {
		throw std::runtime_error("Native query returned no result");
	}
	if (result->HasError()) {
		result->GetErrorObject().Throw();
	}
}

static void Run(Connection &connection, const string &sql) {
	Check(connection.Query(sql));
}

static int64_t Stock(Connection &connection) {
	auto result = connection.Query("SELECT stock::BIGINT FROM transaction_flow_products WHERE id=?", Value::INTEGER(1));
	Check(result);
	auto rows = result->Fetch();
	if (!rows || rows->size() != 1) throw std::runtime_error("Expected one stock row");
	return rows->GetValue(0, 0).GetValue<int64_t>();
}

static int64_t Count(Connection &connection) {
	auto result = connection.Query("SELECT count(*) FROM transaction_flow_products");
	Check(result);
	return result->GetValue<int64_t>(0, 0);
}

static void ChangeStock(Connection &connection, int32_t delta) {
	auto prepared = connection.Prepare("UPDATE transaction_flow_products SET stock=stock+? WHERE id=?");
	Check(prepared);
	vector<Value> values {Value::INTEGER(delta), Value::INTEGER(1)};
	Check(prepared->Execute(values, false));
}

static void BestEffortRollback(Connection &connection) {
	try {
		if (!connection.IsAutoCommit()) {
			connection.Rollback();
		}
	} catch (...) {
		// A lost COMMIT reply remains uncertain; cleanup never replays a write.
	}
}

static void Emit(bool passed, const std::vector<Observation> &observations,
                 const string &phase, const string &error, const string &rows) {
	std::cout << "{\"passed\":" << (passed ? "true" : "false")
	          << ",\"client\":\"native C++ duckdb::Connection\",\"native_used\":true,\"python_used\":false"
	          << ",\"library_version\":" << JsonString(DuckDB::LibraryVersion()) << ",\"sessions\":2,\"checks\":[";
	for (idx_t i = 0; i < observations.size(); i++) {
		if (i) std::cout << ',';
		const auto &item = observations[i];
		std::cout << "{\"name\":" << JsonString(item.name) << ",\"expected\":" << item.expected
		          << ",\"actual\":" << item.actual << ",\"passed\":" << (item.passed ? "true" : "false") << '}';
	}
	std::cout << ']';
	if (passed) {
		std::cout << ",\"rows_before_cleanup\":" << rows << ",\"temporary_table_removed\":true";
	} else {
		std::cout << ",\"phase\":" << JsonString(phase) << ",\"error\":" << JsonString(error);
	}
	std::cout << '}' << std::endl;
}
} // namespace

int main() {
	string token, phase = "connect", final_rows;
	std::vector<Observation> observations;
	auto expect = [&](const string &name, const string &expected, const string &actual, bool passed) {
		phase = name;
		observations.push_back({name, expected, actual, passed});
		if (!passed) throw std::runtime_error("Unexpected observation at " + name);
	};
	try {
		const char *endpoint_env = std::getenv("DUCKONWIRE_ENDPOINT");
		const char *token_env = std::getenv("DUCKONWIRE_TOKEN");
		if (!token_env || !*token_env) throw std::runtime_error("Set DUCKONWIRE_TOKEN");
		token = token_env;
		const string endpoint = endpoint_env && *endpoint_env ? endpoint_env : "quack:127.0.0.1:9495";
		DuckDB database(nullptr);
		Connection writer(database), reader(database);
		auto builtin = writer.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack')"
		                            " AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
		Check(builtin);
		const auto extensions = builtin->GetValue<int64_t>(0, 0);
		expect("builtin_extensions_default", "2", std::to_string(extensions), extensions == 2);
		Run(writer, "CONNECT " + Quote(endpoint) + " (TOKEN " + Quote(token) + ")");
		Run(reader, "CONNECT " + Quote(endpoint) + " (TOKEN " + Quote(token) + ")");
		expect("independent_sessions_autocommit", "true", writer.IsAutoCommit() && reader.IsAutoCommit() ? "true" : "false",
		       writer.IsAutoCommit() && reader.IsAutoCommit());
		bool fixture_owned = false;
		try {
			phase = "fixture";
			Run(writer, "CREATE TABLE transaction_flow_products"
			            "(id INTEGER PRIMARY KEY,name VARCHAR NOT NULL,price DECIMAL(18,2),stock INTEGER NOT NULL)");
			fixture_owned = true;
			auto insert = writer.Prepare("INSERT INTO transaction_flow_products VALUES (?,?,?,?)");
			Check(insert);
			vector<Value> cafe {Value::INTEGER(1), Value("Cafe"), Value::DECIMAL(int64_t(1999), 18, 2), Value::INTEGER(3)};
			Check(insert->Execute(cafe, false)); // No BEGIN: this insert commits automatically.
			const auto initial = Stock(reader);
			expect("autocommit_typed_insert_visible", "3", std::to_string(initial), initial == 3);

			phase = "begin_reader_snapshot";
			reader.BeginTransaction();
			const auto first_read = Stock(reader);
			expect("reader_snapshot_initial", "3", std::to_string(first_read), first_read == 3);
			writer.BeginTransaction();
			vector<Value> arroz {Value::INTEGER(2), Value("Arroz"), Value::DECIMAL(int64_t(850), 18, 2), Value::INTEGER(10)};
			Check(insert->Execute(arroz, false));
			ChangeStock(writer, 1);
			auto changed = writer.Query("SELECT id,name,price,stock FROM transaction_flow_products ORDER BY id");
			Check(changed);
			const auto two_rows = changed->RowCount();
			const auto changed_stock = changed->GetValue<int32_t>(3, 0);
			const bool typed_prices = changed->GetTypes()[2] == LogicalType::DECIMAL(18, 2) &&
			                          changed->GetValue(2, 0) == cafe[2] && changed->GetValue(2, 1) == arroz[2];
			Check(writer.Query("DELETE FROM transaction_flow_products WHERE id=?", Value::INTEGER(2)));
			const auto remaining = Count(writer);
			const string crud = "[" + std::to_string(two_rows) + ',' + std::to_string(changed_stock) + ',' + std::to_string(remaining) + ']';
			expect("typed_crud_inside_writer_transaction", "[2,4,1]", crud,
			       two_rows == 2 && changed_stock == 4 && remaining == 1 && typed_prices);
			const auto pending_read = Stock(reader);
			expect("reader_sees_committed_stock_during_pending_write", "3", std::to_string(pending_read), pending_read == 3);
			writer.Commit();
			const auto retained = Stock(reader);
			expect("reader_retains_snapshot_after_writer_commit", "3", std::to_string(retained), retained == 3);
			reader.Commit();
			reader.BeginTransaction();
			const auto fresh = Stock(reader);
			expect("new_transaction_sees_committed_stock", "4", std::to_string(fresh), fresh == 4);
			reader.Commit();

			phase = "rollback";
			writer.BeginTransaction();
			ChangeStock(writer, 95);
			const auto own_pending = Stock(writer);
			const auto hidden_pending = Stock(reader);
			writer.Rollback();
			const auto rolled_back = Stock(reader);
			expect("rollback_preserves_committed_stock", "[99,4,4]",
			       "[" + std::to_string(own_pending) + ',' + std::to_string(hidden_pending) + ',' + std::to_string(rolled_back) + ']',
			       own_pending == 99 && hidden_pending == 4 && rolled_back == 4);

			phase = "conflict";
			reader.BeginTransaction();
			if (Stock(reader) != 4) throw std::runtime_error("Conflict reader snapshot mismatch");
			writer.BeginTransaction();
			ChangeStock(writer, 5);
			const auto started = std::chrono::steady_clock::now();
			const auto during_write = Stock(reader);
			const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			std::ostringstream progress;
			progress << std::setprecision(9) << "{\"stock\":" << during_write << ",\"seconds\":" << seconds << '}';
			expect("reader_progress_while_other_writer_has_pending_row", "{\"stock\":4,\"seconds_less_than\":2}",
			       progress.str(), during_write == 4 && seconds < 2);
			auto conflict = reader.Query("UPDATE transaction_flow_products SET stock=stock+? WHERE id=?",
			                             Value::INTEGER(7), Value::INTEGER(1));
			const bool rejected = conflict && conflict->HasError() && conflict->GetErrorObject().Type() == ExceptionType::TRANSACTION &&
			                      (conflict->GetError().find("Conflict") != string::npos || conflict->GetError().find("conflict") != string::npos);
			expect("same_row_write_conflict", "\"TRANSACTION conflict\"", rejected ? "\"TRANSACTION conflict\"" : "\"missing conflict\"", rejected);
			reader.Rollback();
			writer.Rollback();
			const auto recovered = Stock(reader);
			expect("connections_recover_after_conflict_rollback", "4", std::to_string(recovered),
			       recovered == 4 && reader.IsAutoCommit() && writer.IsAutoCommit());
			auto final_result = reader.Query("SELECT json_array(id,name,price::VARCHAR,stock) FROM transaction_flow_products ORDER BY id");
			Check(final_result);
			if (final_result->RowCount() != 1) throw std::runtime_error("Final row count differs");
			final_rows = '[' + final_result->GetValue(0, 0).GetValue<string>() + ']';
			expect("committed_final_row_after_all_rollbacks", "[[1,\"Cafe\",\"19.99\",4]]", final_rows,
			       final_rows == "[[1,\"Cafe\",\"19.99\",4]]");
			phase = "cleanup";
			Run(writer, "DROP TABLE transaction_flow_products");
			fixture_owned = false;
			auto absent = reader.Query("SELECT count(*) FROM information_schema.tables WHERE table_name=?", Value("transaction_flow_products"));
			Check(absent);
			auto absent_rows = absent->Fetch();
			if (!absent_rows || absent_rows->size() != 1) throw std::runtime_error("Expected one cleanup count");
			const auto count = absent_rows->GetValue(0, 0).GetValue<int64_t>();
			expect("temporary_table_removed", "0", std::to_string(count), count == 0);
		} catch (...) {
			BestEffortRollback(reader);
			BestEffortRollback(writer);
			if (fixture_owned) {
				try { Run(writer, "DROP TABLE IF EXISTS transaction_flow_products"); } catch (...) {}
			}
			throw;
		}
		Emit(true, observations, phase, "", final_rows);
		return 0;
	} catch (const std::exception &exception) {
		string error = exception.what();
		if (!token.empty()) {
			for (auto offset = error.find(token); offset != string::npos; offset = error.find(token, offset)) {
				error.replace(offset, token.size(), "[REDACTED]");
				offset += string("[REDACTED]").size();
			}
		}
		Emit(false, observations, phase, error, "[]");
		return 1;
	}
}
