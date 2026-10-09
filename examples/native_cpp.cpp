#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace duckdb;

static string Literal(const string &value) {
	string result = "'";
	for (auto c : value) {
		result += c;
		if (c == '\'') {
			result += c;
		}
	}
	return result + "'";
}

static void Require(bool success, const string &message) {
	if (!success) {
		throw std::runtime_error(message);
	}
}

template <class RESULT>
static void Check(RESULT &result) {
	Require(bool(result), "native query returned no result");
	if (result->HasError()) {
		result->GetErrorObject().Throw();
	}
}

static void Run(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	Check(result);
}

static int64_t Count(Connection &con) {
	auto result = con.Query("SELECT count(*) FROM codex_native_cpp_probe");
	Check(result);
	return result->GetValue<int64_t>(0, 0);
}

static string Label(Connection &con) {
	auto result = con.Query("SELECT label FROM codex_native_cpp_probe WHERE id=1");
	Check(result);
	return result->GetValue(0, 0).GetValue<string>();
}

static void OpenRemote(Connection &con, const string &endpoint, const string &token) {
	Run(con, "CONNECT " + Literal(endpoint) + " (TOKEN " + Literal(token) + ")");
}

int main(int argc, char **argv) {
	string token;
	try {
		Require(argc == 2, "usage: native_cpp private-descriptor.json");
		auto started = std::chrono::steady_clock::now();
		DBConfig config;
		config.SetOptionByName("threads", Value::BIGINT(2));
		DuckDB database(nullptr, &config);
		Connection bootstrap(database);
		auto builtin = bootstrap.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'");
		Check(builtin);
		Require(builtin->GetValue<int64_t>(0, 0) == 2, "extensions must be built in and loaded by default");
		// The native JSON extension reads this private local descriptor; no Python process handles SQL or tokens.
		auto descriptor = bootstrap.Query("SELECT endpoint, token FROM read_json_auto(?)", Value(argv[1]));
		Check(descriptor);
		auto chunk = descriptor->Fetch();
		Require(chunk && chunk->size() == 1, "descriptor must contain exactly one endpoint");
		const auto endpoint = chunk->GetValue(0, 0).GetValue<string>();
		token = chunk->GetValue(1, 0).GetValue<string>();
		Connection writer(database), reader(database);
		OpenRemote(writer, endpoint, token);
		OpenRemote(reader, endpoint, token);
		Run(writer, "DROP TABLE IF EXISTS codex_native_cpp_probe");
		Run(writer, "CREATE TABLE codex_native_cpp_probe (id BIGINT PRIMARY KEY, label VARCHAR, amount DECIMAL(18,4), "
		            "enabled BOOLEAN, items INTEGER[], attrs STRUCT(note VARCHAR,n INTEGER))");
		auto insert = writer.Prepare("INSERT INTO codex_native_cpp_probe VALUES (?, ?, ?, ?, ?, ?)");
		Check(insert);
		Require(insert->GetParameterCount() == 6, "native prepared parameter metadata mismatch");
		const auto original_label = string("C++ café ' ? $name");
		const auto amount = Value::DECIMAL(int64_t(123456789), 18, 4);
		const auto items = Value::LIST(LogicalType::INTEGER, {Value::INTEGER(3), Value(), Value::INTEGER(5)});
		const auto attrs = Value::STRUCT({{"note", Value("native λ")}, {"n", Value::INTEGER(7)}});
		vector<Value> values {Value::BIGINT(1), Value(original_label), amount, Value::BOOLEAN(true), items, attrs};
		writer.BeginTransaction();
		Require(!writer.context->RemoteIsAutoCommit(), "remote BEGIN did not start a transaction");
		Require(!writer.IsAutoCommit(), "C++ IsAutoCommit did not report remote transaction state");
		auto inserted = insert->Execute(values, false);
		Check(inserted);
		Require(Count(reader) == 0, "other native session saw an uncommitted insert");
		writer.Commit();
		Require(writer.context->RemoteIsAutoCommit(), "remote COMMIT did not end the transaction");
		Require(Count(reader) == 1, "other native session missed a committed insert");
		auto typed = reader.Query("SELECT id,label,amount,enabled,items,attrs FROM codex_native_cpp_probe");
		Check(typed);
		Require(typed->GetTypes()[2] == LogicalType::DECIMAL(18, 4), "native decimal result type changed");
		Require(typed->GetValue(2, 0) == amount && typed->GetValue(4, 0) == items && typed->GetValue(5, 0) == attrs,
		        "native typed parameter/result round trip changed values");
		Require(Label(reader) == original_label, "quoted Unicode parameter changed");
		reader.BeginTransaction();
		Require(Label(reader) == original_label, "initial snapshot mismatch");
		auto update = writer.Prepare("UPDATE codex_native_cpp_probe SET label=$Label WHERE id=$Id RETURNING id");
		Check(update);
		identifier_map_t<BoundParameterData> named;
		named.emplace(Identifier("label"), BoundParameterData(Value("updated from C++")));
		named.emplace(Identifier("ID"), BoundParameterData(Value::BIGINT(1)));
		writer.BeginTransaction();
		auto updated = update->Execute(named, false);
		Check(updated);
		writer.Commit();
		Require(Label(reader) == original_label, "reader snapshot changed during another session commit");
		reader.Rollback();
		Require(Label(reader) == "updated from C++", "reader did not observe the newer committed update");
		writer.BeginTransaction();
		values[0] = Value::BIGINT(2);
		auto rolled_back_insert = insert->Execute(values, false);
		Check(rolled_back_insert);
		Require(Count(reader) == 1, "reader saw rollback candidate");
		writer.Rollback();
		Require(Count(reader) == 1, "rolled-back native insert persisted");
		writer.BeginTransaction();
		Run(writer, "DELETE FROM codex_native_cpp_probe WHERE id=1");
		Require(Count(writer) == 0 && Count(reader) == 1, "delete isolation mismatch");
		writer.Rollback();
		Require(Count(reader) == 1, "rolled-back native delete persisted");
		writer.BeginTransaction();
		values[0] = Value::BIGINT(3);
		auto scratch_insert = insert->Execute(values, false);
		Check(scratch_insert);
		writer.Commit();
		Require(Count(reader) == 2, "second committed row missing");
		Run(writer, "DELETE FROM codex_native_cpp_probe WHERE id=3");
		Require(Count(reader) == 1, "committed native DELETE not visible");
		writer.SetAutoCommit(false);
		Require(!writer.IsAutoCommit(), "SetAutoCommit(false) did not begin remotely");
		values[0] = Value::BIGINT(4);
		auto autocommit_insert = insert->Execute(values, false);
		Check(autocommit_insert);
		Require(Count(reader) == 1, "SetAutoCommit(false) write escaped transaction");
		writer.SetAutoCommit(true);
		Require(writer.IsAutoCommit() && Count(reader) == 2, "SetAutoCommit(true) did not commit remotely");
		Run(writer, "DELETE FROM codex_native_cpp_probe WHERE id=4");
		bool cross_language = false;
		auto common = writer.Query("SELECT count(*) FROM information_schema.tables WHERE table_name='cross_language_probe'");
		Check(common);
		if (common->GetValue<int64_t>(0, 0) > 0) {
			auto python_row = writer.Query("SELECT label FROM cross_language_probe WHERE id=0");
			Check(python_row);
			Require(python_row->RowCount() == 1 && python_row->GetValue(0, 0).GetValue<string>() == "from Python",
			        "C++ did not observe the Python committed row");
			auto common_insert = writer.Prepare("INSERT INTO cross_language_probe VALUES (?, ?)");
			Check(common_insert);
			vector<Value> common_values {Value::BIGINT(1), Value("from C++")};
			auto common_result = common_insert->Execute(common_values, false);
			Check(common_result);
			cross_language = true;
		}
		auto multilingual = writer.Query("SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'");
		Check(multilingual);
		if (multilingual->GetValue<int64_t>(0, 0) > 0) {
			auto marker = writer.Prepare("INSERT INTO lang_multiclient_probe VALUES (?, ?)");
			Check(marker);
			vector<Value> marker_values {Value::INTEGER(1), Value("from C++")};
			auto written = marker->Execute(marker_values, false);
			Check(written);
			auto changed = written->Fetch();
			Require(changed && changed->GetValue(0, 0).GetValue<int64_t>() == 1, "C++ marker affected-row count");
		}
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		std::cout << "{\"passed\":true,\"client\":\"native C++ duckdb::Connection\",\"python_used\":false,"
		             "\"library_version\":\"" << DuckDB::LibraryVersion() << "\","
		             "\"sessions\":2,\"typed_prepared_positional\":true,\"typed_prepared_named\":true,"
		             "\"complex_types\":true,\"commit_visibility\":true,\"snapshot_isolation\":true,"
		             "\"rollback\":true,\"crud\":true,\"rows_left\":1,\"seconds\":" << seconds
		          << ",\"cross_language_python_to_cpp_to_python\":" << (cross_language ? "true" : "false")
		          << ",\"cpp_Get_SetAutoCommit_remote_verified\":true"
		          << "}" << std::endl;
		reader.context->CloseRemoteConnections();
		writer.context->CloseRemoteConnections();
		return 0;
	} catch (const std::exception &error) {
		string message = error.what();
		if (!token.empty()) {
			for (auto offset = message.find(token); offset != string::npos; offset = message.find(token)) {
				message.replace(offset, token.size(), "[REDACTED]");
			}
		}
		std::cerr << "native C++ probe failed: " << message << std::endl;
		return 1;
	}
}
