#pragma once

#include "duckdb.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#ifdef __APPLE__
// DuckDB's __restrict macro conflicts with the C99 array declarations in Apple's spawn.h.
#pragma push_macro("__restrict")
#undef __restrict
#define __restrict
#include <spawn.h>
#pragma pop_macro("__restrict")
#else
#include <spawn.h>
#endif
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace test_support {
namespace fs = std::filesystem;

inline void Require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

template <class RESULT> inline void Check(const RESULT &result) {
	Require(bool(result), "DuckDB returned no result");
	if (result->HasError()) result->GetErrorObject().Throw();
}

inline std::vector<std::vector<duckdb::Value>> FetchRows(duckdb::QueryResult &result) {
	std::vector<std::vector<duckdb::Value>> rows;
	while (auto chunk = result.Fetch()) {
		for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
			std::vector<duckdb::Value> values;
			for (duckdb::idx_t column = 0; column < chunk->ColumnCount(); ++column)
				values.push_back(chunk->GetValue(column, row));
			rows.push_back(std::move(values));
		}
	}
	return rows;
}

inline std::string Literal(const std::string &value) {
	std::string result = "'";
	for (char byte : value) {
		result += byte;
		if (byte == '\'') result += byte;
	}
	return result + "'";
}

inline void Connect(duckdb::Connection &connection, const std::string &endpoint, const std::string &token) {
	auto result = connection.Query("CONNECT " + Literal(endpoint) + " (TOKEN " + Literal(token) + ")");
	Check(result);
}

class TempRoot {
public:
	explicit TempRoot(const fs::path &parent = fs::temp_directory_path()) {
		fs::create_directories(parent);
		auto pattern = (fs::canonical(parent) / "duckonwire-native-test-XXXXXX").string();
		std::vector<char> bytes(pattern.begin(), pattern.end());
		bytes.push_back('\0');
		const char *created = mkdtemp(bytes.data());
		if (!created) throw std::runtime_error("Cannot create owned native test fixture");
		path_ = created;
	}
	TempRoot(const TempRoot &) = delete;
	TempRoot &operator=(const TempRoot &) = delete;
	~TempRoot() {
		std::error_code error;
		fs::remove_all(path_, error);
	}
	const fs::path &path() const { return path_; }
private:
	fs::path path_;
};

inline int AvailablePort() {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) throw std::runtime_error("Cannot allocate native fixture port");
	struct sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
		close(fd);
		throw std::runtime_error("Cannot bind native fixture port");
	}
	socklen_t size = sizeof(address);
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
		close(fd);
		throw std::runtime_error("Cannot inspect native fixture port");
	}
	const int port = ntohs(address.sin_port);
	close(fd);
	return port;
}

// This fixture launches the real C++ server. SQL stays in DuckDB's native Connection.
class NativeServer {
public:
	NativeServer(const fs::path &root, const fs::path &database, const fs::path &executable, int port = 0,
	             bool init_demo = false, const std::vector<std::string> &extra_arguments = {})
	    : root_(fs::canonical(root)), database_(fs::weakly_canonical(database)),
	      executable_(fs::canonical(executable)) {
		Require(access(executable_.c_str(), X_OK) == 0, "Native server executable is unavailable");
		Require(database_.lexically_relative(root_).native().find("..") != 0,
		        "Native fixture database must be inside its root");
		fs::create_directories(database_.parent_path());
		const int selected_port = port ? port : AvailablePort();
		endpoint_ = "quack:127.0.0.1:" + std::to_string(selected_port);
		state_ = root_ / ("server-" + std::to_string(selected_port) + ".json");
		Require(!fs::exists(state_), "Native fixture descriptor already exists; preserved");
		int pipes[2];
		if (pipe(pipes) != 0) throw std::runtime_error("Cannot create native server readiness pipe");
		for (int fd : pipes) fcntl(fd, F_SETFD, FD_CLOEXEC);
		std::vector<std::string> words {executable_.string(), "--root", root_.string(),
		    "--database", database_.string(), "--state", state_.string(), "--port", std::to_string(selected_port), "--pg-port", "0"};
		if (init_demo) words.push_back("--init-demo");
		words.insert(words.end(), extra_arguments.begin(), extra_arguments.end());
		std::vector<char *> arguments;
		for (auto &word : words) arguments.push_back(word.data());
		arguments.push_back(nullptr);
		std::vector<char *> environment;
		for (char **item = environ; *item; ++item) {
			if (std::strncmp(*item, "DUCKONWIRE_TOKEN=", 17) != 0) environment.push_back(*item);
		}
		environment.push_back(nullptr);
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
		posix_spawn_file_actions_addclose(&actions, pipes[0]);
		posix_spawn_file_actions_addclose(&actions, pipes[1]);
		const int error = posix_spawn(&child_, executable_.c_str(), &actions, nullptr, arguments.data(), environment.data());
		posix_spawn_file_actions_destroy(&actions);
		close(pipes[1]);
		if (error) {
			close(pipes[0]);
			child_ = -1;
			throw std::runtime_error("Cannot spawn native server: " + std::string(std::strerror(error)));
		}
		output_ = pipes[0];
		fcntl(output_, F_SETFL, fcntl(output_, F_GETFL) | O_NONBLOCK);
		try {
			WaitReady();
			ReadDescriptor();
		} catch (...) {
			stop();
			throw;
		}
	}
	NativeServer(const NativeServer &) = delete;
	NativeServer &operator=(const NativeServer &) = delete;
	~NativeServer() { stop(); }
	const std::string &endpoint() const { return endpoint_; }
	int pg_port() const { return pg_port_; }
	const std::string &token() const { return token_; }
	pid_t pid() const { return child_; }
	const fs::path &state() const { return state_; }
	bool forced_stop() const { return forced_stop_; }
	int exit_status() const { return status_; }
	bool clean_shutdown() const { return !forced_stop_ && WIFEXITED(status_) && WEXITSTATUS(status_) == 0; }

	void stop() noexcept {
		if (child_ > 0) {
			pid_t waited = WaitChild();
			if (waited == 0) {
				kill(child_, SIGTERM);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
				while ((waited = WaitChild()) == 0 && std::chrono::steady_clock::now() < deadline) {
					Drain();
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				if (waited == 0) {
					forced_stop_ = true;
					kill(child_, SIGKILL);
					do { waited = waitpid(child_, &status_, 0); } while (waited < 0 && errno == EINTR);
				}
			}
			// Only remove our own stale descriptor after our child is gone (e.g. a deliberate SIGKILL).
			if (waited == child_ || (waited < 0 && errno == ECHILD)) {
				if (descriptor_owned_) {
					struct stat current {};
					if (lstat(state_.c_str(), &current) == 0 && current.st_dev == descriptor_.st_dev &&
					    current.st_ino == descriptor_.st_ino) unlink(state_.c_str());
				}
			}
			child_ = -1;
		}
		if (output_ >= 0) { close(output_); output_ = -1; }
	}

private:
	pid_t WaitChild() noexcept {
		pid_t waited;
		do { waited = waitpid(child_, &status_, WNOHANG); } while (waited < 0 && errno == EINTR);
		return waited;
	}
	void Drain() noexcept {
		if (output_ < 0) return;
		char bytes[4096];
		ssize_t count;
		while ((count = read(output_, bytes, sizeof(bytes))) > 0) {
			try {
				messages_.append(bytes, static_cast<size_t>(count));
				if (messages_.size() > 65536) messages_.erase(0, messages_.size() - 65536);
			} catch (...) {}
		}
	}
	void WaitReady() {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline) {
			Drain();
			if (messages_.find("\"ready\":true") != std::string::npos) return;
			const auto waited = WaitChild();
			if (waited == child_ || waited < 0) throw std::runtime_error("Native server exited before readiness: " + messages_);
			struct pollfd descriptor {output_, POLLIN, 0};
			poll(&descriptor, 1, 100);
		}
		throw std::runtime_error("Native server readiness exceeded 30 seconds");
	}
	void ReadDescriptor() {
		Require(lstat(state_.c_str(), &descriptor_) == 0 && S_ISREG(descriptor_.st_mode) &&
		        (descriptor_.st_mode & 0777) == 0600, "Native server descriptor is not private");
		descriptor_owned_ = true;
		duckdb::DBConfig config;
		config.SetOptionByName("threads", duckdb::Value::BIGINT(2));
		duckdb::DuckDB database(nullptr, &config);
		duckdb::Connection reader(database);
		auto result = reader.Query("SELECT endpoint,token,pid,pg_port FROM read_json_auto(?)", duckdb::Value(state_.string()));
		Check(result);
		const auto rows = FetchRows(*result);
		Require(rows.size() == 1, "Native descriptor must contain one server");
		Require(rows[0][0].GetValue<std::string>() == endpoint_ &&
		        rows[0][2].GetValue<int64_t>() == child_, "Native descriptor differs from our child");
		token_ = rows[0][1].GetValue<std::string>();
		pg_port_ = rows[0][3].GetValue<int>();
		Require(pg_port_ > 0 && pg_port_ <= 65535, "Native fixture PostgreSQL port is invalid");
		Require(!token_.empty(), "Native fixture server token is empty");
	}
	fs::path root_, database_, executable_, state_;
	std::string endpoint_, token_, messages_;
	pid_t child_ = -1;
	int output_ = -1, status_ = 0, pg_port_ = 0;
	bool descriptor_owned_ = false, forced_stop_ = false;
	struct stat descriptor_ {};
};
} // namespace test_support
