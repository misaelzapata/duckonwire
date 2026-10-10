#include "duckdb.hpp"
#include "duckonwire/json.hpp"
#include "native_server.hpp"

#include <iostream>

using namespace duckdb;
using namespace test_support;

namespace {
struct ChildResult { int status; std::string output; };
ChildResult RunClient(const fs::path &program, const NativeServer &server, bool with_token) {
	std::string endpoint = "DUCKONWIRE_ENDPOINT=" + server.endpoint();
	std::string token = "DUCKONWIRE_TOKEN=" + server.token();
	std::vector<char *> environment;
	for (char **item = environ; *item; ++item) {
		if (std::strncmp(*item, "DUCKONWIRE_ENDPOINT=", 20) != 0 && std::strncmp(*item, "DUCKONWIRE_TOKEN=", 17) != 0)
			environment.push_back(*item);
	}
	environment.push_back(endpoint.data());
	if (with_token) environment.push_back(token.data());
	environment.push_back(nullptr);
	const auto path = fs::canonical(program).string();
	char *arguments[] = {const_cast<char *>(path.c_str()), nullptr};
	int pipes[2]; Require(pipe(pipes) == 0, "Cannot create example output pipe");
	for (int fd : pipes) fcntl(fd, F_SETFD, FD_CLOEXEC);
	posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipes[0]);
	posix_spawn_file_actions_addclose(&actions, pipes[1]);
	pid_t child = -1;
	const auto error = posix_spawn(&child, path.c_str(), &actions, nullptr, arguments, environment.data());
	posix_spawn_file_actions_destroy(&actions); close(pipes[1]);
	if (error) { close(pipes[0]); throw std::runtime_error("Cannot run ATTACH example"); }
	fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	std::string output; int status = 0;
	while (true) {
		char buffer[4096]; ssize_t length;
		while ((length = read(pipes[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<size_t>(length));
		if (output.size() > 65536) {
			kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			close(pipes[0]); throw std::runtime_error("ATTACH example output exceeded its bound");
		}
		auto done = waitpid(child, &status, WNOHANG);
		if (done == child) {
			while ((length = read(pipes[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<size_t>(length));
			break;
		}
		if (done < 0 || std::chrono::steady_clock::now() >= deadline) {
			kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			close(pipes[0]); throw std::runtime_error("ATTACH example did not finish within its deadline");
		}
		pollfd descriptor {pipes[0], POLLIN, 0}; poll(&descriptor, 1, 50);
	}
	close(pipes[0]);
	return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, std::move(output)};
}
int64_t Scalar(Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql); Check(result);
	return result->GetValue<int64_t>(0, 0);
}
void ValidateExample(Connection &local, const std::string &output) {
	auto result = local.Query("SELECT json_valid(?), json_extract_string(?, '$.passed'), json_array_length(?,'$.checks'), json_extract_string(?, '$.native_used'), json_extract_string(?, '$.source_id')",
	                          Value(output), Value(output), Value(output), Value(output), Value(output)); Check(result);
	auto row = result->Fetch();
	Require(row && row->size() == 1 && row->GetValue(0, 0).GetValue<bool>() && row->GetValue(1, 0).ToString() == "true" &&
	        row->GetValue(2, 0).GetValue<int64_t>() == 8 && row->GetValue(3, 0).ToString() == "true" &&
	        row->GetValue(4, 0).ToString() == DuckDB::SourceID(), "ATTACH example did not report all native checks");
	auto checks = local.Query("SELECT count(*) FROM json_each(?, '$.checks') WHERE json_extract_string(value,'$.passed')='true'", Value(output)); Check(checks);
	auto counts = checks->Fetch();
	Require(counts && counts->size() == 1 && counts->GetValue(0, 0).GetValue<int64_t>() == 8, "ATTACH example reported a failed check");
}
} // namespace

int main(int argc, char **argv) {
	fs::path server_binary, client_binary, parent = fs::temp_directory_path();
	for (int i = 1; i < argc; ++i) {
		const std::string option = argv[i];
		if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
		if (option == "--server") server_binary = argv[++i];
		else if (option == "--client") client_binary = argv[++i];
		else if (option == "--fixture-parent") parent = argv[++i];
		else { std::cerr << "Unknown option\n"; return 2; }
	}
	std::vector<std::string> checks;
	std::string phase = "configuration";
	try {
		Require(!server_binary.empty() && !client_binary.empty(), "--server and --client are required");
		TempRoot fixture(parent);
		NativeServer server(fixture.path(), fixture.path() / "attach.duckdb", server_binary);
		DBConfig config; config.SetOptionByName("threads", Value::BIGINT(2));
		DuckDB database(nullptr, &config); Connection local(database), observer(database);
		Connect(observer, server.endpoint(), server.token());
		Check(observer.Query("CREATE TABLE attach_preserved(id INTEGER PRIMARY KEY, label VARCHAR)"));
		Check(observer.Query("INSERT INTO attach_preserved VALUES(?,?)", Value::INTEGER(42), Value("existing data")));
		phase = "missing_token_fails_before_remote_mutation";
		auto missing = RunClient(client_binary, server, false);
		Require(missing.status == 2 && missing.output.find("DUCKONWIRE_TOKEN is required") != std::string::npos, "Missing credential was accepted");
		checks.push_back(phase);
		for (int run = 1; run <= 2; ++run) {
			phase = "actual_attach_example_run_" + std::to_string(run);
			auto result = RunClient(client_binary, server, true);
			Require(result.status == 0, "Native ATTACH example failed"); ValidateExample(local, result.output);
			Require(result.output.find(server.token()) == std::string::npos && result.output.find(server.endpoint()) == std::string::npos, "Connection settings leaked in output");
			checks.push_back(phase);
			phase = "owned_remote_fixture_removed_after_run_" + std::to_string(run);
			Require(Scalar(observer, "SELECT count(*) FROM information_schema.tables WHERE table_name LIKE 'duckonwire_attach_%'") == 0, "Remote fixture table remains");
			checks.push_back(phase);
		}
		phase = "preexisting_remote_data_preserved";
		Require(Scalar(observer, "SELECT count(*) FROM attach_preserved WHERE id=42 AND label='existing data'") == 1, "Unowned remote data changed");
		Check(observer.Query("DROP TABLE attach_preserved")); checks.push_back(phase);
		Check(observer.Query("CREATE TABLE attach_sessions(id INTEGER PRIMARY KEY, stock INTEGER, name VARCHAR)"));
		Check(observer.Query("INSERT INTO attach_sessions VALUES(?,?,?)", Value::INTEGER(1), Value::INTEGER(3), Value("Coffee 'dark roast'")));
		Connection writer(database), reader(database);
		Check(writer.Query("ATTACH " + Literal(server.endpoint()) + " AS session_fixture (TOKEN " + Literal(server.token()) + ")"));
		phase = "attach_unbound_parameter_fails_closed";
		auto unresolved = writer.Prepare("SELECT stock FROM session_fixture.main.attach_sessions WHERE name=?");
		Require(unresolved && unresolved->HasError() && unresolved->GetError().find("Values were not provided for the following parameters: 1") != std::string::npos,
		        "ATTACH parameter limitation changed; review its binding support"); unresolved.reset();
		Require(Scalar(writer, "SELECT count(*) FROM session_fixture.main.attach_sessions") == 1, "ATTACH session failed after its parameter error"); checks.push_back(phase);
		Check(writer.Query("CONNECT session_fixture")); Check(reader.Query("CONNECT session_fixture"));
		phase = "connect_bound_string_with_apostrophe";
		auto selected = writer.Query("SELECT stock FROM attach_sessions WHERE name=?", Value("Coffee 'dark roast'")); Check(selected);
		auto selected_row = selected->Fetch();
		Require(selected_row && selected_row->size() == 1 && selected_row->GetValue(0, 0).GetValue<int64_t>() == 3, "CONNECT changed a bound string"); selected.reset(); checks.push_back(phase);
		phase = "connect_alias_sessions_are_independent";
		reader.BeginTransaction(); Require(Scalar(reader, "SELECT stock FROM attach_sessions") == 3, "Initial snapshot mismatch");
		writer.BeginTransaction();
		Check(writer.Query("UPDATE attach_sessions SET stock=? WHERE id=?", Value::INTEGER(4), Value::INTEGER(1)));
		Require(Scalar(writer, "SELECT stock FROM attach_sessions") == 4 && Scalar(reader, "SELECT stock FROM attach_sessions") == 3, "Independent alias sessions leaked pending writes");
		checks.push_back(phase);
		phase = "connect_alias_reader_retains_snapshot";
		writer.Commit(); Require(Scalar(reader, "SELECT stock FROM attach_sessions") == 3, "Reader snapshot changed after writer commit");
		reader.Commit(); reader.BeginTransaction(); Require(Scalar(reader, "SELECT stock FROM attach_sessions") == 4, "Fresh transaction missed writer commit"); reader.Commit();
		checks.push_back(phase);
		phase = "connect_alias_close_rolls_back_pending_write";
		{
			Connection abandoned(database); Check(abandoned.Query("CONNECT session_fixture")); abandoned.BeginTransaction();
			Check(abandoned.Query("UPDATE attach_sessions SET stock=? WHERE id=?", Value::INTEGER(55), Value::INTEGER(1)));
		}
		Require(Scalar(reader, "SELECT stock FROM attach_sessions") == 4, "Closed alias session committed pending work"); checks.push_back(phase);
		Check(writer.Query("DROP TABLE attach_sessions"));
		Check(reader.Query("DISCONNECT")); Check(writer.Query("DISCONNECT")); Check(writer.Query("DETACH session_fixture"));
		phase = "owned_server_graceful_shutdown";
		server.stop(); Require(server.clean_shutdown(), "Owned native server shutdown failed"); checks.push_back(phase);
		std::cout << "{\"passed\":true,\"native_used\":true,\"example_checks_per_run\":8,\"example_runs\":2,\"checks\":[";
		for (size_t i = 0; i < checks.size(); ++i) {
			if (i) std::cout << ',';
			std::cout << "{\"name\":" << duckonwire::JsonString(checks[i]) << ",\"passed\":true}";
		}
		std::cout << "]}\n";
		return 0;
	} catch (...) {
		std::cout << "{\"passed\":false,\"phase\":" << duckonwire::JsonString(phase)
		          << ",\"error\":\"Native ATTACH acceptance failed; connection settings are not logged\"}\n";
		return 1;
	}
}
