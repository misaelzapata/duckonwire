#include "duckonwire/json.hpp"
#include "native_server.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

namespace {
using Bytes = std::vector<unsigned char>;
using test_support::Require;

void U16(Bytes &out, std::uint16_t value) {
	out.push_back(static_cast<unsigned char>(value >> 8));
	out.push_back(static_cast<unsigned char>(value));
}
void U32(Bytes &out, std::uint32_t value) {
	for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>(value >> shift));
}
std::uint32_t Read32(const Bytes &in, std::size_t at) {
	Require(at + 4 <= in.size(), "Truncated backend integer");
	std::uint32_t value = 0;
	for (int i = 0; i < 4; ++i) value = (value << 8) | in[at + i];
	return value;
}
void String(Bytes &out, const std::string &value) {
	out.insert(out.end(), value.begin(), value.end()); out.push_back(0);
}
struct Reply { char type; Bytes body; };
std::string ErrorCode(const Reply &reply) {
	if (reply.type != 'E') return {};
	std::size_t at = 0;
	while (at < reply.body.size() && reply.body[at]) {
		const auto field = reply.body[at++];
		auto end = std::find(reply.body.begin() + at, reply.body.end(), 0);
		Require(end != reply.body.end(), "Unterminated backend error field");
		std::string value(reply.body.begin() + at, end);
		at = static_cast<std::size_t>(end - reply.body.begin()) + 1;
		if (field == 'C') return value;
	}
	return {};
}
bool HasError(const std::vector<Reply> &replies, const std::string &code) {
	for (const auto &reply : replies) if (ErrorCode(reply) == code) return true;
	return false;
}
bool Contains(const std::vector<Reply> &replies, char type) {
	for (const auto &reply : replies) if (reply.type == type) return true;
	return false;
}
std::string Command(const std::vector<Reply> &replies) {
	std::string command;
	for (const auto &reply : replies) if (reply.type == 'C') {
		Require(!reply.body.empty() && reply.body.back() == 0, "Invalid CommandComplete string");
		command.assign(reply.body.begin(), reply.body.end() - 1);
	}
	return command;
}
std::vector<std::pair<std::string, std::uint32_t>> Description(const std::vector<Reply> &replies) {
	for (const auto &reply : replies) if (reply.type == 'T') {
		Require(reply.body.size() >= 2, "Truncated RowDescription");
		const auto columns = (unsigned(reply.body[0]) << 8) | reply.body[1]; std::size_t at = 2;
		std::vector<std::pair<std::string, std::uint32_t>> result;
		for (unsigned i = 0; i < columns; ++i) {
			auto end = std::find(reply.body.begin() + at, reply.body.end(), 0);
			Require(end != reply.body.end(), "Unterminated RowDescription name");
			std::string name(reply.body.begin() + at, end); at = static_cast<std::size_t>(end - reply.body.begin()) + 1;
			Require(at + 18 <= reply.body.size(), "Truncated RowDescription column fields");
			result.emplace_back(name, Read32(reply.body, at + 6)); at += 18;
		}
		Require(at == reply.body.size(), "Trailing RowDescription bytes"); return result;
	}
	return {};
}
std::vector<std::vector<std::optional<std::string>>> Rows(const std::vector<Reply> &replies) {
	std::vector<std::vector<std::optional<std::string>>> rows;
	for (const auto &reply : replies) {
		if (reply.type != 'D') continue;
		Require(reply.body.size() >= 2, "Truncated backend row");
		std::size_t at = 2;
		const auto columns = (unsigned(reply.body[0]) << 8) | reply.body[1];
		std::vector<std::optional<std::string>> row;
		for (unsigned i = 0; i < columns; ++i) {
			const auto length = static_cast<std::int32_t>(Read32(reply.body, at)); at += 4;
			if (length == -1) { row.emplace_back(); continue; }
			Require(length >= 0 && static_cast<std::size_t>(length) <= reply.body.size() - at, "Invalid backend field size");
			row.emplace_back(std::string(reply.body.begin() + at, reply.body.begin() + at + length)); at += length;
		}
		Require(at == reply.body.size(), "Trailing backend row bytes"); rows.push_back(std::move(row));
	}
	return rows;
}
char State(const std::vector<Reply> &replies) {
	Require(!replies.empty() && replies.back().type == 'Z' && replies.back().body.size() == 1, "Missing ReadyForQuery state");
	return static_cast<char>(replies.back().body[0]);
}

class Wire {
public:
	explicit Wire(int port) {
		fd_ = socket(AF_INET, SOCK_STREAM, 0); Require(fd_ >= 0, "Cannot create protocol socket");
		timeval deadline {5, 0};
		setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
		setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
#ifdef SO_NOSIGPIPE
		int no_signal = 1; setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
		sockaddr_in address {}; address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
			close(fd_); fd_ = -1; throw std::runtime_error("Cannot connect to protocol fixture");
		}
	}
	Wire(const Wire &) = delete;
	Wire &operator=(const Wire &) = delete;
	~Wire() { Close(); }
	void Close() { if (fd_ >= 0) { shutdown(fd_, SHUT_RDWR); close(fd_); fd_ = -1; } }
	void Send(const Bytes &bytes) {
		std::size_t at = 0;
		while (at < bytes.size()) {
#ifdef MSG_NOSIGNAL
			auto sent = send(fd_, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
#else
			auto sent = send(fd_, bytes.data() + at, bytes.size() - at, 0);
#endif
			if (sent < 0 && errno == EINTR) continue;
			Require(sent > 0, "Protocol send failed or timed out"); at += static_cast<std::size_t>(sent);
		}
	}
	void Message(char type, const Bytes &body = {}) {
		Bytes frame {static_cast<unsigned char>(type)}; U32(frame, static_cast<std::uint32_t>(body.size() + 4));
		frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
	}
	bool Receive(void *output, std::size_t size) {
		auto bytes = static_cast<unsigned char *>(output); std::size_t at = 0;
		while (at < size) {
			auto count = recv(fd_, bytes + at, size - at, 0);
			if (count < 0 && errno == EINTR) continue;
			if (!count && !at) return false;
			Require(count > 0, "Protocol receive failed or timed out"); at += static_cast<std::size_t>(count);
		}
		return true;
	}
	std::optional<Reply> Next() {
		unsigned char type; if (!Receive(&type, 1)) return {};
		Bytes header(4); Require(Receive(header.data(), 4), "Missing backend header");
		const auto length = Read32(header, 0);
		Require(length >= 4 && length <= 16 * 1024 * 1024, "Invalid backend frame length");
		Reply reply {static_cast<char>(type), Bytes(length - 4)};
		if (!reply.body.empty()) Require(Receive(reply.body.data(), reply.body.size()), "Missing backend frame body");
		return reply;
	}
	std::vector<Reply> Until(const std::string &terminal) {
		std::vector<Reply> replies;
		for (std::size_t i = 0; i < 50000; ++i) {
			auto reply = Next(); Require(bool(reply), "Backend disconnected before response completion");
			const auto type = reply->type; replies.push_back(std::move(*reply));
			if (terminal.find(type) != std::string::npos) return replies;
		}
		throw std::runtime_error("Backend response exceeds test message bound");
	}
	void Startup() {
		Bytes body; U32(body, 196608); String(body, "user"); String(body, "duckonwire");
		String(body, "database"); String(body, "duckonwire"); String(body, "client_encoding"); String(body, "UTF8"); body.push_back(0);
		Bytes frame; U32(frame, static_cast<std::uint32_t>(body.size() + 4)); frame.insert(frame.end(), body.begin(), body.end()); Send(frame);
		auto auth = Next(); Require(auth && auth->type == 'R' && Read32(auth->body, 0) == 3, "Missing password challenge");
	}
	void Password(const std::string &password) { Bytes body; String(body, password); Message('p', body); }
	void Login(const std::string &password) {
		Startup(); Password(password); const auto replies = Until("Z");
		Require(!Contains(replies, 'E') && State(replies) == 'I', "Protocol login failed");
		for (const auto &reply : replies) if (reply.type == 'K') {
			Require(reply.body.size() == 8, "Invalid backend cancellation key"); id = Read32(reply.body, 0); secret = Read32(reply.body, 4);
		}
		Require(id != 0, "Missing backend cancellation key");
	}
	void QuerySend(const std::string &sql) { Bytes body; String(body, sql); Message('Q', body); }
	std::vector<Reply> Query(const std::string &sql) { QuerySend(sql); return Until("Z"); }
	void Parse(const std::string &name, const std::string &sql, const std::vector<std::uint32_t> &oids = {}) {
		Bytes body; String(body, name); String(body, sql); U16(body, static_cast<std::uint16_t>(oids.size()));
		for (auto oid : oids) U32(body, oid); Message('P', body);
	}
	void Bind(const std::string &portal, const std::string &statement,
	          const std::vector<std::optional<std::string>> &values = {}, bool binary_output = false, bool binary_input = false) {
		Bytes body; String(body, portal); String(body, statement); U16(body, binary_input ? 1 : 0);
		if (binary_input) U16(body, 1);
		U16(body, static_cast<std::uint16_t>(values.size()));
		for (const auto &value : values) {
			U32(body, value ? static_cast<std::uint32_t>(value->size()) : UINT32_MAX);
			if (value) body.insert(body.end(), value->begin(), value->end());
		}
		U16(body, binary_output ? 1 : 0); if (binary_output) U16(body, 1); Message('B', body);
	}
	void Execute(const std::string &portal, std::uint32_t maximum = 0) { Bytes body; String(body, portal); U32(body, maximum); Message('E', body); }
	void Describe(char kind, const std::string &name) { Bytes body {static_cast<unsigned char>(kind)}; String(body, name); Message('D', body); }
	void CloseObject(char kind, const std::string &name) { Bytes body {static_cast<unsigned char>(kind)}; String(body, name); Message('C', body); }
	std::uint32_t id = 0, secret = 0;
private:
	int fd_ = -1;
};

std::int64_t Scalar(Wire &wire, const std::string &sql) {
	const auto replies = wire.Query(sql);
	Require(!Contains(replies, 'E'), "Scalar query failed"); const auto rows = Rows(replies);
	Require(rows.size() == 1 && rows[0].size() == 1 && rows[0][0], "Scalar result shape mismatch");
	return std::stoll(*rows[0][0]);
}
} // namespace

int main(int argc, char **argv) {
	std::filesystem::path server_program, fixture_parent = std::filesystem::temp_directory_path();
	for (int i = 1; i < argc; ++i) {
		std::string option = argv[i];
		if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
		if (option == "--server") server_program = argv[++i];
		else if (option == "--fixture-parent") fixture_parent = argv[++i];
		else { std::cerr << "Unknown option\n"; return 2; }
	}
	std::vector<std::pair<std::string, std::string>> checks;
	std::string private_token;
	try {
		Require(!server_program.empty(), "--server is required");
		test_support::TempRoot fixture(fixture_parent);
		test_support::NativeServer server(fixture.path(), fixture.path() / "protocol.duckdb", server_program);
		private_token = server.token();
		auto client = [&] { auto wire = std::make_unique<Wire>(server.pg_port()); wire->Login(server.token()); return wire; };
		auto test = [&](const std::string &name, const std::function<void()> &body) {
			try { body(); checks.emplace_back(name, ""); }
			catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
		};
		test("password_failure_is_fatal_and_bounded", [&] {
			Wire bad(server.pg_port()); bad.Startup(); bad.Password("deliberately-wrong-password");
			auto reply = bad.Next(); Require(reply && ErrorCode(*reply) == "28P01", "Authentication SQLSTATE mismatch");
			Require(!bad.Next(), "Rejected authentication did not close socket");
		});
		test("ssl_declined_then_plain_startup", [&] {
			Wire wire(server.pg_port()); Bytes request; U32(request, 8); U32(request, 80877103); wire.Send(request);
			unsigned char denied = 0; Require(wire.Receive(&denied, 1) && denied == 'N', "SSL negotiation mismatch");
			wire.Login(server.token()); Require(Scalar(wire, "SELECT 42") == 42, "Query after SSL negotiation failed");
		});
		test("malformed_frame_fails_closed_without_affecting_other_session", [&] {
			auto wire = client(); Bytes malformed {'Q'}; U32(malformed, 3); wire->Send(malformed);
			auto reply = wire->Next(); Require(reply && ErrorCode(*reply) == "08P01", "Malformed frame SQLSTATE mismatch");
			Require(!wire->Next(), "Malformed frame did not close socket");
			auto healthy = client(); Require(Scalar(*healthy, "SELECT 7") == 7, "Malformed frame affected server");
		});
		test("parser_error_and_sync_recovery", [&] {
			auto wire = client(); wire->Parse("bad", "SELCT 1"); wire->Parse("skipped", "SELECT 99"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(HasError(replies, "42601") && State(replies) == 'I', "Parse error recovery mismatch");
			Require(!Contains(replies, '1') && Scalar(*wire, "SELECT 8") == 8, "Messages after error were not skipped until Sync");
		});
		test("unsupported_parameter_oid_rejected_even_for_null", [&] {
			auto wire = client(); wire->Parse("unknown_oid", "SELECT $1::INTEGER", {999999});
			wire->Bind("unknown_portal", "unknown_oid", {std::nullopt}); wire->Execute("unknown_portal"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(HasError(replies, "0A000") && !Contains(replies, 'D'), "Unsupported OID was accepted with NULL");
			Require(Scalar(*wire, "SELECT 9") == 9, "Unsupported OID poisoned session");
		});
		test("text_parameter_unicode_null_decimal_and_blob", [&] {
			auto wire = client(); wire->Parse("typed", "SELECT $1::VARCHAR,$2::INTEGER,$3::DECIMAL(18,2),$4::BLOB,$5::DECIMAL(18,4)", {25,23,1700,17,1700});
			wire->Bind("typed_portal", "typed", {std::string("Cafe 日本"),std::nullopt,std::string("19.99"),std::string("\\x0001ff"),std::string("1e-2")});
			wire->Describe('P', "typed_portal"); wire->Execute("typed_portal"); wire->Message('S');
			auto replies = wire->Until("Z"); const auto rows = Rows(replies);
			Require(!Contains(replies, 'E') && rows.size() == 1 && rows[0].size() == 5, "Typed parameter query failed");
			Require(rows[0][0] == "Cafe 日本" && !rows[0][1] && rows[0][2] == "19.99" && rows[0][3] == "\\x0001ff" && rows[0][4] == "0.0100", "Typed parameter values changed, including exponent numeric");
			wire->Parse("uint32", "SELECT $1::UINTEGER AS max_unsigned", {26});
			wire->Bind("uint32", "uint32", {std::string(4, char(0xff))}, true, true);
			wire->Describe('P', "uint32"); wire->Execute("uint32"); wire->Message('S');
			const auto unsigned_replies = wire->Until("Z"); const auto unsigned_columns = Description(unsigned_replies); const auto unsigned_rows = Rows(unsigned_replies);
			Require(!Contains(unsigned_replies, 'E') && unsigned_columns.size() == 1 && unsigned_columns[0].second == 26, "UINTEGER result did not advertise PostgreSQL OID26");
			Require(unsigned_rows.size() == 1 && unsigned_rows[0].size() == 1 && unsigned_rows[0][0] && unsigned_rows[0][0]->size() == 4 && Read32(Bytes(unsigned_rows[0][0]->begin(), unsigned_rows[0][0]->end()), 0) == UINT32_MAX, "Binary OID26 lost uint32 maximum");
			for (const auto &reply : unsigned_replies) if (reply.type == 'T') {
				auto name_end = std::find(reply.body.begin() + 2, reply.body.end(), 0); const auto size_at = static_cast<std::size_t>(name_end - reply.body.begin()) + 11;
				Require(size_at + 2 <= reply.body.size() && reply.body[size_at] == 0 && reply.body[size_at + 1] == 4, "OID26 RowDescription type width is not four bytes");
			}
		});
		test("prepared_binary_pagination_15003_rows", [&] {
			auto wire = client(); Require(State(wire->Query("BEGIN")) == 'T', "BEGIN state mismatch");
			wire->Parse("paged", "SELECT i::INTEGER FROM range(15003) t(i)"); wire->Bind("page", "paged", {}, true);
			wire->Describe('P', "page"); wire->Execute("page", 3); wire->Message('H');
			auto replies = wire->Until("sCE"); Require(Contains(replies, 's') && !Contains(replies, 'E'), "Initial portal page was not suspended");
			std::size_t count = 0;
			auto consume = [&](const std::vector<Reply> &part) {
				for (const auto &row : Rows(part)) {
					Require(row.size() == 1 && row[0] && row[0]->size() == 4, "Binary integer row shape mismatch");
					Bytes bytes(row[0]->begin(), row[0]->end()); Require(Read32(bytes, 0) == count++, "Paging lost or repeated a row");
				}
			};
			consume(replies);
			for (int page = 0; page < 4 && !Contains(replies, 'C'); ++page) {
				wire->Execute("page", 5000); wire->Message('H'); replies = wire->Until("sCE");
				Require(!Contains(replies, 'E'), "Resumed portal execution failed"); consume(replies);
			}
			Require(count == 15003 && Contains(replies, 'C'), "Portal did not complete exact row count");
			wire->Message('S'); Require(State(wire->Until("Z")) == 'T', "Sync closed explicit transaction");
			Require(State(wire->Query("COMMIT")) == 'I', "COMMIT state mismatch");
		});
		test("close_statement_closes_associated_portals", [&] {
			auto wire = client(); wire->Query("BEGIN"); wire->Parse("to_close", "SELECT 1"); wire->Bind("associated", "to_close");
			wire->CloseObject('S', "to_close"); wire->Execute("associated"); wire->Message('S');
			auto replies = wire->Until("Z"); Require(Contains(replies, '3') && HasError(replies, "34000") && !Contains(replies, 'D'), "Closed statement left its portal executable");
			wire->Query("ROLLBACK");
		});
		test("explicit_constraint_failure_requires_rollback", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_unique(id INTEGER PRIMARY KEY)"); wire->Query("INSERT INTO pgwire_review_unique VALUES (1)");
			wire->Query("BEGIN"); auto failed = wire->Query("INSERT INTO pgwire_review_unique VALUES (1)");
			Require(HasError(failed, "23505") && State(failed) == 'E', "Constraint error transaction state mismatch");
			Require(HasError(wire->Query("SELECT 1"), "25P02"), "Failed transaction allowed ordinary query");
			Require(State(wire->Query("ROLLBACK")) == 'I' && Scalar(*wire, "SELECT count(*) FROM pgwire_review_unique") == 1, "ROLLBACK failed recovery");
		});
		test("extended_implicit_transaction_commits_only_at_sync", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_pipeline(id INTEGER)"); auto writer = client();
			writer->Parse("pending", "INSERT INTO pgwire_review_pipeline VALUES (1)"); writer->Bind("pending", "pending"); writer->Execute("pending"); writer->Message('H');
			Require(!Contains(writer->Until("CE"), 'E'), "Pipeline INSERT failed");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline") == 0, "INSERT committed before Sync");
			writer->Message('S'); Require(State(writer->Until("Z")) == 'I', "Implicit Sync state mismatch");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline") == 1, "Sync did not commit pending INSERT");
		});
		test("extended_implicit_error_rolls_back_prior_commands", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_pipeline_error(id INTEGER)"); auto writer = client();
			writer->Parse("insert", "INSERT INTO pgwire_review_pipeline_error VALUES (1)"); writer->Bind("insert", "insert"); writer->Execute("insert");
			writer->Parse("fail", "SELECT * FROM pgwire_review_missing_table"); writer->Message('S');
			auto replies = writer->Until("Z"); Require(Contains(replies, 'E') && State(replies) == 'I', "Failed implicit pipeline did not recover at Sync");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_pipeline_error") == 0, "Failed pipeline committed earlier INSERT");
		});
		test("disconnect_rolls_back_pending_explicit_write", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_close(id INTEGER PRIMARY KEY)");
			{ auto writer = client(); writer->Query("BEGIN"); writer->Query("INSERT INTO pgwire_review_close VALUES (1)"); }
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_close") == 0, "Disconnected transaction became visible");
			Require(!Contains(admin->Query("INSERT INTO pgwire_review_close VALUES (1)"), 'E'), "Disconnected transaction retained primary-key write");
			Require(Scalar(*admin, "SELECT count(*) FROM pgwire_review_close") == 1, "Reused key did not persist after disconnect rollback");
		});
		test("mvcc_reader_keeps_snapshot_while_writer_commits", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_snapshot(id INTEGER PRIMARY KEY,stock INTEGER)");
			admin->Query("INSERT INTO pgwire_review_snapshot VALUES (1,3)"); auto reader = client(); auto writer = client();
			Require(State(reader->Query("BEGIN")) == 'T' && Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 3, "Initial reader snapshot mismatch");
			writer->Query("BEGIN"); Require(!Contains(writer->Query("UPDATE pgwire_review_snapshot SET stock=4 WHERE id=1"), 'E'), "Independent writer was blocked");
			Require(State(writer->Query("COMMIT")) == 'I', "Writer COMMIT failed");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 3, "Committed write changed existing reader snapshot");
			reader->Query("COMMIT"); reader->Query("BEGIN");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_snapshot") == 4, "New transaction did not see committed writer"); reader->Query("COMMIT");
		});
		test("same_row_conflict_preserves_reader_and_rollback_recovers", [&] {
			auto admin = client(); admin->Query("CREATE TABLE pgwire_review_conflict(id INTEGER PRIMARY KEY,stock INTEGER)");
			admin->Query("INSERT INTO pgwire_review_conflict VALUES (1,3)"); auto first = client(); auto second = client(); auto reader = client();
			first->Query("BEGIN"); second->Query("BEGIN"); first->Query("UPDATE pgwire_review_conflict SET stock=4 WHERE id=1");
			auto conflict = second->Query("UPDATE pgwire_review_conflict SET stock=5 WHERE id=1");
			Require(HasError(conflict, "40001") && State(conflict) == 'E', "Same-row conflict SQLSTATE/state mismatch");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_conflict") == 3, "Pending conflicting write blocked or altered reader");
			Require(State(second->Query("ROLLBACK")) == 'I', "Conflicted transaction did not roll back"); first->Query("COMMIT");
			Require(Scalar(*reader, "SELECT stock FROM pgwire_review_conflict") == 4 && Scalar(*second, "SELECT stock FROM pgwire_review_conflict") == 4, "Conflict changed committed stock or poisoned session");
		});
		test("simple_batch_begin_promotes_prior_implicit_work", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_simple_transaction(id INTEGER)");
			const auto replies = wire->Query("INSERT INTO pgwire_review_simple_transaction VALUES (1); BEGIN; INSERT INTO pgwire_review_simple_transaction VALUES (2); ROLLBACK");
			Require(!Contains(replies, 'E') && State(replies) == 'I', "Simple transaction-control batch failed");
			Require(Scalar(*wire, "SELECT count(*) FROM pgwire_review_simple_transaction") == 0, "Statement before BEGIN escaped batch rollback");
		});
		test("dml_command_tags_and_returning_row_counts", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_tags(id INTEGER PRIMARY KEY,stock INTEGER)");
			auto check = [&](const std::string &sql, const std::string &tag, std::size_t rows) {
				const auto replies = wire->Query(sql);
				Require(!Contains(replies, 'E') && Command(replies) == tag, "DML CommandComplete tag or count mismatch");
				Require(Rows(replies).size() == rows, "DML produced an unexpected result row count");
				if (!rows) Require(!Contains(replies, 'T'), "Non-RETURNING DML exposed internal Count column");
			};
			check("INSERT INTO pgwire_review_tags VALUES (1,3),(2,3),(3,3)", "INSERT 0 3", 0);
			check("UPDATE pgwire_review_tags SET stock=stock+1 WHERE id<=2", "UPDATE 2", 0);
			check("DELETE FROM pgwire_review_tags WHERE id=3", "DELETE 1", 0);
			check("INSERT INTO pgwire_review_tags VALUES (4,3),(5,3) RETURNING id", "INSERT 0 2", 2);
			check("UPDATE pgwire_review_tags SET stock=stock+1 WHERE id<=2 RETURNING id", "UPDATE 2", 2);
			check("DELETE FROM pgwire_review_tags WHERE id IN (4,5) RETURNING id", "DELETE 2", 2);
			wire->Parse("insert_tag", "INSERT INTO pgwire_review_tags VALUES ($1,3)", {23});
			wire->Bind("insert_tag", "insert_tag", {std::string("7")}); wire->Describe('P', "insert_tag"); wire->Execute("insert_tag"); wire->Message('S');
			const auto prepared = wire->Until("Z");
			Require(!Contains(prepared, 'E') && Contains(prepared, 'n') && !Contains(prepared, 'D') && Command(prepared) == "INSERT 0 1", "Prepared DML tag/NoData mismatch");
		});
		test("schema_changes_are_fresh_across_pgwire_and_quack", [&] {
			auto wire = client(); wire->Query("CREATE TABLE pgwire_review_schema(id INTEGER)"); wire->Query("INSERT INTO pgwire_review_schema VALUES (1)");
			Require(Scalar(*wire, "SELECT count(*) FROM pg_range") == 0 && Scalar(*wire, "SELECT count(*) FROM pg_enum") == 0, "Wire catalog advertised unsupported range/enum codecs");
			duckdb::DuckDB local(nullptr); duckdb::Connection quack(local); test_support::Connect(quack, server.endpoint(), server.token());
			auto persistent_catalog = quack.Query("SELECT count(*) FROM duckdb_views() WHERE NOT internal AND NOT temporary AND view_name IN ('pg_type','pg_namespace','pg_range','pg_enum')"); test_support::Check(persistent_catalog);
			Require(persistent_catalog->GetValue(0, 0).GetValue<std::int64_t>() == 0, "Wire catalog views leaked into persistent database");
			test_support::Check(quack.Query("SELECT id FROM pgwire_review_schema"));
			Require(!Contains(wire->Query("ALTER TABLE pgwire_review_schema ADD COLUMN label VARCHAR DEFAULT 'ready'"), 'E'), "PGwire ALTER failed");
			auto label = quack.Query("SELECT label FROM pgwire_review_schema"); test_support::Check(label);
			Require(label->GetValue(0, 0).GetValue<std::string>() == "ready", "Quack retained stale schema after PGwire ALTER");
			test_support::Check(quack.Query("ALTER TABLE pgwire_review_schema ADD COLUMN stock INTEGER DEFAULT 7"));
			Require(Scalar(*wire, "SELECT stock FROM pgwire_review_schema") == 7, "PGwire retained stale schema after Quack ALTER");
		});
		test("bound_file_table_functions_describe_actual_schema", [&] {
			const auto csv = fixture.path() / "parameterized.csv"; const auto blob = fixture.path() / "parameterized.bin";
			{ std::ofstream file(csv); file << "id,name,stock\n1,Cafe,3\n2,Arroz,10\n"; Require(bool(file), "Cannot create owned CSV fixture"); }
			{ std::ofstream file(blob, std::ios::binary); const char bytes[] {'\0','\1',char(0xff)}; file.write(bytes, 3); Require(bool(file), "Cannot create owned blob fixture"); }
			auto wire = client(); wire->Parse("csv_schema", "SELECT * FROM read_csv($1::VARCHAR, header=true)", {25});
			wire->Bind("csv_schema", "csv_schema", {csv.string()}); wire->Describe('P', "csv_schema"); wire->Execute("csv_schema"); wire->Message('S');
			const auto csv_replies = wire->Until("Z"); const auto columns = Description(csv_replies); const auto csv_rows = Rows(csv_replies);
			Require(!Contains(csv_replies, 'E') && columns.size() == 3 && columns[0].first == "id" && columns[1].first == "name" && columns[2].first == "stock", "Bound CSV Describe returned stale or unknown columns");
			Require(csv_rows.size() == 2 && csv_rows[0].size() == 3 && csv_rows[0][0] == "1" && csv_rows[0][1] == "Cafe" && csv_rows[0][2] == "3" && csv_rows[1][0] == "2" && csv_rows[1][1] == "Arroz" && csv_rows[1][2] == "10", "Bound CSV Execute values mismatch");
			wire->Parse("blob_schema", "SELECT content,size FROM read_blob($1::VARCHAR)", {25});
			wire->Bind("blob_schema", "blob_schema", {blob.string()}); wire->Describe('P', "blob_schema"); wire->Execute("blob_schema"); wire->Message('S');
			const auto blob_replies = wire->Until("Z"); const auto blob_columns = Description(blob_replies); const auto blob_rows = Rows(blob_replies);
			Require(!Contains(blob_replies, 'E') && blob_columns.size() == 2 && blob_columns[0].first == "content" && blob_columns[0].second == 17 && blob_columns[1].first == "size", "Bound blob Describe returned stale or unknown columns");
			Require(blob_rows.size() == 1 && blob_rows[0].size() == 2 && blob_rows[0][0] == "\\x0001ff" && blob_rows[0][1] == "3", "Bound blob Execute values mismatch");
		});
		test("describe_only_insert_does_not_execute_or_end_transaction", [&] {
			auto observer = client(); observer->Query("CREATE TABLE pgwire_review_describe(id INTEGER PRIMARY KEY)"); auto writer = client();
			writer->Parse("never_execute", "INSERT INTO pgwire_review_describe VALUES ($1) RETURNING id", {23});
			writer->Bind("never_execute", "never_execute", {std::string("1")}); writer->Describe('P', "never_execute"); writer->Message('S');
			const auto no_execute = writer->Until("Z");
			Require(!Contains(no_execute, 'E') && Description(no_execute).size() == 1 && State(no_execute) == 'I', "Describe-only INSERT metadata or state mismatch");
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_review_describe") == 0, "Describe-only INSERT mutated autocommit database");
			writer->Query("BEGIN"); writer->Query("INSERT INTO pgwire_review_describe VALUES (2)");
			writer->Parse("pending_describe", "INSERT INTO pgwire_review_describe VALUES ($1) RETURNING id", {23});
			writer->Bind("pending_describe", "pending_describe", {std::string("3")}); writer->Describe('P', "pending_describe"); writer->Message('H');
			const auto described = writer->Until("TE"); Require(!Contains(described, 'E') && Description(described).size() == 1, "Explicit transaction Describe failed");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_review_describe") == 0, "Describe committed previous explicit transaction write");
			writer->CloseObject('P', "pending_describe"); writer->Message('S'); Require(State(writer->Until("Z")) == 'T', "Closing described portal ended explicit transaction");
			Require(Scalar(*writer, "SELECT count(*) FROM pgwire_review_describe") == 1, "Describe executed pending INSERT or discarded previous write");
			writer->Query("COMMIT"); Require(Scalar(*observer, "SELECT sum(id) FROM pgwire_review_describe") == 2, "Describe changed committed transaction contents");
		});
		test("statement_describe_declared_types_and_unresolved_fail_closed", [&] {
			auto wire = client(); wire->Parse("declared_integer", "SELECT $1", {23}); wire->Describe('S', "declared_integer");
			wire->Bind("declared_integer", "declared_integer", {std::string("42")}, true); wire->Execute("declared_integer"); wire->Message('S');
			const auto integer_replies = wire->Until("Z"); const auto integer_columns = Description(integer_replies); const auto integer_rows = Rows(integer_replies);
			Require(!Contains(integer_replies, 'E') && integer_columns.size() == 1 && integer_columns[0].second == 23, "Statement Describe advertised wrong declared integer type");
			Require(integer_rows.size() == 1 && integer_rows[0].size() == 1 && integer_rows[0][0] && integer_rows[0][0]->size() == 4 && Read32(Bytes(integer_rows[0][0]->begin(), integer_rows[0][0]->end()), 0) == 42, "Statement-cached binary integer metadata disagrees with actual value");
			wire->Bind("declared_null", "declared_integer", {std::nullopt}, true); wire->Describe('P', "declared_null"); wire->Execute("declared_null"); wire->Message('S');
			const auto null_replies = wire->Until("Z"); const auto null_columns = Description(null_replies); const auto null_rows = Rows(null_replies);
			Require(!Contains(null_replies, 'E') && null_columns.size() == 1 && null_columns[0].second == 23 && null_rows.size() == 1 && null_rows[0].size() == 1 && !null_rows[0][0], "Declared NULL lost integer type or cached metadata compatibility");
			wire->Parse("unresolved", "SELECT $1"); wire->Describe('S', "unresolved"); wire->Message('S');
			const auto unresolved = wire->Until("Z"); Require(HasError(unresolved, "0A000") && !Contains(unresolved, 'T') && State(unresolved) == 'I', "Unresolved Statement Describe did not fail closed");
			wire->Parse("unbound_file", "SELECT * FROM read_blob($1::VARCHAR)", {25}); wire->Describe('S', "unbound_file"); wire->Message('S');
			const auto no_file = wire->Until("Z"); Require(HasError(no_file, "0A000") && !Contains(no_file, 'T') && State(no_file) == 'I', "Dynamic Statement Describe invented metadata without a bound filename");
			wire->Parse("explicit_cast", "SELECT $1::INTEGER"); wire->Describe('S', "explicit_cast"); wire->Bind("explicit_cast", "explicit_cast", {std::string("43")}, true); wire->Execute("explicit_cast"); wire->Message('S');
			const auto recovered = wire->Until("Z"); Require(!Contains(recovered, 'E') && Description(recovered).size() == 1 && Description(recovered)[0].second == 23 && Rows(recovered).size() == 1, "Explicit cast did not recover unresolved metadata session");
			wire->Parse("profiled", "EXPLAIN ANALYZE SELECT sum(i) FROM range($1::BIGINT) t(i)", {20});
			wire->Bind("profiled", "profiled", {std::string("1000")}); wire->Describe('P', "profiled"); wire->Execute("profiled"); wire->Message('S');
			const auto profiled = wire->Until("Z"); const auto profile_rows = Rows(profiled);
			Require(!Contains(profiled, 'E') && Description(profiled).size() == 2 && profile_rows.size() == 1 && profile_rows[0].size() == 2 && profile_rows[0][1], "Prepared EXPLAIN ANALYZE failed");
			Require(profile_rows[0][1]->find("Total Time:") != std::string::npos && profile_rows[0][1]->find("profiling is disabled") == std::string::npos, "Prepared EXPLAIN ANALYZE lost native profiling");
			Require(State(profiled) == 'I' && Scalar(*wire, "SELECT 44") == 44, "Prepared EXPLAIN ANALYZE left session unusable");
		});
		test("sql_cursor_requires_explicit_transaction", [&] {
			auto wire = client();
			const auto replies = wire->Query("DECLARE outside_cursor CURSOR FOR SELECT 1");
			Require(HasError(replies, "25P01") && State(replies) == 'I', "SQL cursor escaped transaction scope");
			Require(Scalar(*wire, "SELECT 11") == 11, "Rejected SQL cursor poisoned connection");
		});
		test("sql_cursor_forward_pagination_preserves_every_row", [&] {
			auto wire = client(); Require(State(wire->Query("BEGIN")) == 'T', "Cursor BEGIN failed");
			const auto declared = wire->Query("DECLARE page_cursor NO SCROLL CURSOR FOR SELECT i::INTEGER FROM range(15003) t(i)");
			Require(!Contains(declared, 'E') && Command(declared) == "DECLARE CURSOR" && !Contains(declared, 'D'), "Cursor declaration returned rows or failed");
			std::int64_t count = 0;
			for (unsigned page = 0; page < 17; ++page) {
				const auto fetched = wire->Query("FETCH FORWARD 1000 FROM page_cursor");
				Require(!Contains(fetched, 'E') && State(fetched) == 'T', "Cursor FETCH failed or ended transaction");
				const auto rows = Rows(fetched);
				Require(Command(fetched) == "FETCH " + std::to_string(rows.size()), "Cursor fetch command count differs from result");
				for (const auto &row : rows) Require(row.size() == 1 && row[0] && std::stoll(*row[0]) == count++, "Cursor lost, duplicated or reordered a row");
				if (rows.empty()) break;
			}
			Require(count == 15003, "SQL cursor failed complete pagination");
			Require(!Contains(wire->Query("CLOSE page_cursor"), 'E'), "Cursor close failed");
			Require(State(wire->Query("COMMIT")) == 'I', "Cursor COMMIT failed");
		});
		test("sql_cursor_identifier_case_and_duplicate_errors", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE \"Mixed Cursor\" CURSOR FOR SELECT 12"), 'E'), "Quoted cursor identifier rejected");
			const auto row = Rows(wire->Query("FETCH 1 FROM \"Mixed Cursor\""));
			Require(row.size() == 1 && row[0][0] == "12", "Quoted cursor result changed");
			Require(!Contains(wire->Query("DECLARE lower_cursor CURSOR FOR SELECT 13"), 'E'), "Unquoted cursor declaration failed");
			const auto lower = Rows(wire->Query("FETCH 1 FROM LOWER_CURSOR"));
			Require(lower.size() == 1 && lower[0].size() == 1 && lower[0][0] == "13", "Unquoted cursor identifier was not folded");
			const auto duplicate = wire->Query("DECLARE LOWER_CURSOR CURSOR FOR SELECT 99");
			Require(HasError(duplicate, "42P03") && State(duplicate) == 'E', "Duplicate cursor did not abort transaction with correct error");
			wire->Query("ROLLBACK"); Require(Scalar(*wire, "SELECT 14") == 14, "Duplicate cursor rollback failed");
		});
		test("sql_cursor_independent_positions_and_zero_fetch", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE first_cursor CURSOR FOR SELECT i::INTEGER FROM range(3) t(i)"), 'E'), "First cursor declaration failed");
			Require(!Contains(wire->Query("DECLARE second_cursor CURSOR FOR SELECT i::INTEGER FROM range(10,13) t(i)"), 'E'), "Second cursor declaration failed");
			const auto zero = wire->Query("FETCH 0 FROM first_cursor");
			Require(!Contains(zero, 'E') && Rows(zero).empty() && Description(zero).size() == 1 && Command(zero) == "FETCH 0", "Zero FETCH advanced or lost metadata");
			const auto first = Rows(wire->Query("FETCH 1 FROM first_cursor"));
			const auto second = Rows(wire->Query("FETCH 2 FROM second_cursor"));
			Require(first.size() == 1 && first[0][0] == "0" && second.size() == 2 && second[0][0] == "10" && second[1][0] == "11", "Cursor positions interfered");
			const auto remainder = Rows(wire->Query("FETCH ALL FROM first_cursor"));
			Require(remainder.size() == 2 && remainder[0][0] == "1" && remainder[1][0] == "2", "FETCH ALL lost remainder");
			wire->Query("ROLLBACK");
		});
		test("extended_cursor_declaration_binds_values_without_returning_rows", [&] {
			auto wire = client(); wire->Query("BEGIN");
			wire->Parse("declare_bound", "DECLARE bound_cursor CURSOR FOR SELECT $1::INTEGER AS id,$2::VARCHAR AS name,$3::DECIMAL(18,2) AS price", {23,25,1700});
			wire->Bind("declare_bound", "declare_bound", {std::string("15"),std::string("Coffee 'dark roast'"),std::string("19.25")});
			wire->Describe('P', "declare_bound"); wire->Execute("declare_bound"); wire->Message('S');
			const auto declared = wire->Until("Z");
			Require(!Contains(declared, 'E') && Contains(declared, 'n') && !Contains(declared, 'D') && Command(declared) == "DECLARE CURSOR" && State(declared) == 'T', "Extended DECLARE metadata or command mismatch");
			const auto fetched = wire->Query("FETCH 1 FROM bound_cursor"); const auto rows = Rows(fetched); const auto columns = Description(fetched);
			Require(!Contains(fetched, 'E') && rows.size() == 1 && rows[0].size() == 3 && rows[0][0] == "15" && rows[0][1] == "Coffee 'dark roast'" && rows[0][2] == "19.25", "Bound cursor values changed");
			Require(columns.size() == 3 && columns[0].second == 23 && columns[1].second == 25 && columns[2].second == 1700, "Bound cursor lost result types");
			wire->Query("COMMIT"); Require(Scalar(*wire, "SELECT 16") == 16, "Extended DECLARE sent extra ReadyForQuery frames");
		});
		test("extended_fetch_and_close_keep_protocol_synchronized", [&] {
			auto wire = client(); wire->Query("BEGIN");
			Require(!Contains(wire->Query("DECLARE prepared_cursor CURSOR FOR SELECT i::INTEGER FROM range(2) t(i)"), 'E'), "Prepared control cursor setup failed");
			wire->Parse("fetch_cursor", "FETCH 1 FROM prepared_cursor"); wire->Bind("fetch_cursor", "fetch_cursor");
			wire->Describe('P', "fetch_cursor"); wire->Execute("fetch_cursor"); wire->Message('S');
			const auto fetched = wire->Until("Z"); const auto rows = Rows(fetched);
			Require(!Contains(fetched, 'E') && rows.size() == 1 && rows[0][0] == "0" && Command(fetched) == "FETCH 1" && State(fetched) == 'T', "Prepared FETCH failed");
			Require(Scalar(*wire, "SELECT 17") == 17, "Prepared FETCH emitted premature or duplicate ReadyForQuery");
			wire->Parse("close_cursor", "CLOSE prepared_cursor"); wire->Bind("close_cursor", "close_cursor");
			wire->Describe('P', "close_cursor"); wire->Execute("close_cursor"); wire->Message('S');
			const auto closed = wire->Until("Z");
			Require(!Contains(closed, 'E') && Contains(closed, 'n') && Command(closed) == "CLOSE CURSOR" && State(closed) == 'T', "Prepared CLOSE failed");
			Require(Scalar(*wire, "SELECT 18") == 18, "Prepared CLOSE left protocol frames unread"); wire->Query("COMMIT");
		});
		test("sql_cursors_are_removed_at_transaction_end", [&] {
			auto wire = client();
			for (const auto &ending : {std::string("COMMIT"),std::string("ROLLBACK")}) {
				wire->Query("BEGIN"); Require(!Contains(wire->Query("DECLARE end_cursor CURSOR FOR SELECT 19"), 'E'), "Transaction cursor setup failed");
				Require(State(wire->Query(ending)) == 'I', "Cursor transaction did not end"); wire->Query("BEGIN");
				const auto gone = wire->Query("FETCH 1 FROM end_cursor");
				Require(HasError(gone, "34000") && State(gone) == 'E', "Cursor survived transaction end"); wire->Query("ROLLBACK");
			}
			Require(Scalar(*wire, "SELECT 20") == 20, "Cursor cleanup poisoned session");
		});
		test("close_all_preserves_transaction_and_prepared_statements", [&] {
			auto observer = client(); auto wire = client();
			Require(!Contains(wire->Query("CREATE TABLE pgwire_cursor_close_all(id INTEGER)"), 'E'), "CLOSE ALL fixture setup failed");
			wire->Query("BEGIN"); wire->Query("INSERT INTO pgwire_cursor_close_all VALUES(21)");
			wire->Parse("still_prepared", "SELECT $1::INTEGER", {23}); wire->Message('S'); Require(!Contains(wire->Until("Z"), 'E'), "Prepared statement setup failed");
			Require(!Contains(wire->Query("DECLARE close_all_cursor CURSOR FOR SELECT 22"), 'E'), "CLOSE ALL cursor setup failed");
			const auto closed = wire->Query("CLOSE ALL");
			Require(!Contains(closed, 'E') && State(closed) == 'T', "CLOSE ALL reset connection or ended transaction");
			Require(Scalar(*observer, "SELECT count(*) FROM pgwire_cursor_close_all") == 0 && Scalar(*wire, "SELECT count(*) FROM pgwire_cursor_close_all") == 1, "CLOSE ALL committed or discarded pending write");
			wire->Bind("still_prepared", "still_prepared", {std::string("23")}); wire->Execute("still_prepared"); wire->Message('S');
			const auto preserved = wire->Until("Z"); const auto rows = Rows(preserved);
			Require(!Contains(preserved, 'E') && rows.size() == 1 && rows[0][0] == "23", "CLOSE ALL removed prepared statements");
			wire->Query("COMMIT"); Require(Scalar(*observer, "SELECT sum(id) FROM pgwire_cursor_close_all") == 21, "CLOSE ALL changed committed contents");
		});
		test("savepoints_do_not_fake_rollback_for_legacy_writers", [&] {
			auto wire = client(); wire->Query("BEGIN");
			const auto unsupported = wire->Query("SAVEPOINT writer_savepoint");
			Require(HasError(unsupported, "0A000") && State(unsupported) == 'E', "Writer savepoint was accepted without write rollback semantics");
			Require(State(wire->Query("ROLLBACK")) == 'I' && Scalar(*wire, "SELECT 24") == 24, "Rejected writer savepoint did not recover after rollback");
		});
		test("cancel_request_interrupts_query_and_session_recovers", [&] {
			auto wire = client(); wire->QuerySend("SELECT sum(sin(i::DOUBLE)) FROM range(1000000000) t(i)");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			Wire cancel(server.pg_port()); Bytes request; U32(request, 16); U32(request, 80877102); U32(request, wire->id); U32(request, wire->secret); cancel.Send(request);
			const auto replies = wire->Until("Z"); Require(HasError(replies, "57014") && State(replies) == 'I', "Cancel did not interrupt active query");
			Require(!cancel.Next() && Scalar(*wire, "SELECT 10") == 10, "Cancel socket or session recovery mismatch");
		});
		server.stop(); test("graceful_shutdown_with_protocol_sessions", [&] { Require(server.clean_shutdown(), "Native server required forced shutdown"); });
	} catch (const std::exception &error) { checks.emplace_back("fixture_setup", error.what()); }
	bool passed = !checks.empty();
	for (auto &check : checks) {
		passed = passed && check.second.empty();
		if (!private_token.empty()) {
			std::size_t at = 0;
			while ((at = check.second.find(private_token, at)) != std::string::npos) {
				check.second.replace(at, private_token.size(), "[redacted]"); at += 10;
			}
		}
	}
	std::cout << "{\"passed\":" << (passed ? "true" : "false") << ",\"native_used\":true,\"checks\":[";
	for (std::size_t i = 0; i < checks.size(); ++i) {
		if (i) std::cout << ',';
		std::cout << "{\"name\":" << duckonwire::JsonString(checks[i].first) << ",\"passed\":" << (checks[i].second.empty() ? "true" : "false");
		if (!checks[i].second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(checks[i].second);
		std::cout << '}';
	}
	std::cout << "]}\n";
	return passed ? 0 : 1;
}
