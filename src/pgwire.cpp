#include "duckonwire/pgwire.hpp"
#include "duckonwire/scram.hpp"

#include <openssl/ssl.h>
#include <openssl/rand.h>

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <set>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace duckonwire {
namespace {
using Buffer = std::vector<unsigned char>;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::Value;
constexpr std::size_t MAX_FRAME = 16 * 1024 * 1024;
constexpr std::size_t MAX_STARTUP = 64 * 1024;
constexpr std::size_t MAX_OBJECTS = 256;
constexpr std::int64_t PG_EPOCH_MICROS = 946684800000000LL;
constexpr std::int32_t PG_EPOCH_DAYS = 10957;

struct WireError : std::runtime_error {
    std::string code;
    bool fatal;
    WireError(std::string code_p, std::string message, bool fatal_p = false)
        : std::runtime_error(std::move(message)), code(std::move(code_p)), fatal(fatal_p) {}
};
struct Disconnected {};

void Put16(Buffer &out, std::uint16_t value) {
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}
void Put32(Buffer &out, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void Put64(Buffer &out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void PutString(Buffer &out, const std::string &value) {
    if (value.find('\0') != std::string::npos) throw WireError("22021", "A protocol string contains a zero byte");
    out.insert(out.end(), value.begin(), value.end()); out.push_back(0);
}
struct Reader {
    const Buffer &data;
    std::size_t at = 0;
    explicit Reader(const Buffer &data_p) : data(data_p) {}
    void Need(std::size_t count) const {
        if (count > data.size() - at) throw WireError("08P01", "Truncated PostgreSQL protocol message", true);
    }
    std::uint8_t Byte() { Need(1); return data[at++]; }
    std::uint16_t U16() { Need(2); auto value = std::uint16_t(data[at]) << 8 | data[at + 1]; at += 2; return value; }
    std::uint32_t U32() {
        Need(4); std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | data[at++];
        return value;
    }
    std::uint64_t U64() {
        Need(8); std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | data[at++];
        return value;
    }
    std::string String() {
        auto begin = at;
        while (at < data.size() && data[at]) ++at;
        if (at == data.size()) throw WireError("08P01", "Unterminated protocol string", true);
        std::string value(reinterpret_cast<const char *>(data.data() + begin), at - begin); ++at;
        return value;
    }
    Buffer Bytes(std::size_t count) {
        Need(count); Buffer value(data.begin() + at, data.begin() + at + count); at += count; return value;
    }
    void End() const {
        if (at != data.size()) throw WireError("08P01", "Unexpected bytes at end of protocol message", true);
    }
};
struct WireSocket {
    int fd;
    SSL *tls = nullptr; // Worker-owned; Stop only shuts down the underlying descriptor.
    std::chrono::steady_clock::time_point read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool nonblocking = false;
    explicit WireSocket(int descriptor) : fd(descriptor) {}
    operator int() const { return fd; }
    WireSocket &operator=(int descriptor) { fd = descriptor; return *this; }
};
bool WaitSocket(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        int timeout = -1;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) return false;
            timeout = static_cast<int>(std::min<std::int64_t>(INT_MAX, std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1));
        }
        pollfd pending {fd, events, 0}; const auto result = poll(&pending, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pending.revents & events);
    }
}
bool Receive(WireSocket &socket, void *data, std::size_t size) {
    auto out = static_cast<unsigned char *>(data);
    while (size) {
        if (socket.read_deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= socket.read_deadline) return false;
        int count;
        if (socket.tls) {
            count = SSL_read(socket.tls, out, static_cast<int>(std::min<std::size_t>(size, INT_MAX)));
            if (count <= 0) {
                const auto error = SSL_get_error(socket.tls, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    if (!WaitSocket(socket.fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, socket.read_deadline)) return false;
                    continue;
                }
                if (error == SSL_ERROR_SYSCALL && errno == EINTR) continue;
                return false;
            }
        } else {
            count = static_cast<int>(recv(socket.fd, out, size, 0));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && socket.nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!WaitSocket(socket.fd, POLLIN, socket.read_deadline)) return false;
                continue;
            }
            if (count <= 0) return false;
        }
        size -= static_cast<std::size_t>(count); out += count;
    }
    return true;
}
void Send(WireSocket &socket, const void *data, std::size_t size) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto input = static_cast<const unsigned char *>(data);
    while (size) {
        if (std::chrono::steady_clock::now() >= deadline) throw Disconnected();
        int count;
        if (socket.tls) {
            count = SSL_write(socket.tls, input, static_cast<int>(std::min<std::size_t>(size, INT_MAX)));
            if (count <= 0) {
                const auto error = SSL_get_error(socket.tls, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    if (!WaitSocket(socket.fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline)) throw Disconnected();
                    continue;
                }
                if (error == SSL_ERROR_SYSCALL && errno == EINTR) continue;
                throw Disconnected();
            }
        } else {
#ifdef MSG_NOSIGNAL
            count = static_cast<int>(send(socket.fd, input, size, MSG_NOSIGNAL));
#else
            count = static_cast<int>(send(socket.fd, input, size, 0));
#endif
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && socket.nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!WaitSocket(socket.fd, POLLOUT, deadline)) throw Disconnected();
                continue;
            }
            if (count <= 0) throw Disconnected();
        }
        size -= static_cast<std::size_t>(count); input += count;
    }
}
void Message(WireSocket &socket, char type, const Buffer &body = {}) {
    if (body.size() > MAX_FRAME - 4) throw WireError("54000", "Result field exceeds the PGwire frame limit");
    // Keep each protocol message immediately visible, while sending its header
    // and payload in one write instead of two small TLS records.
    Buffer frame;
    frame.reserve(body.size() + 5);
    frame.push_back(static_cast<unsigned char>(type));
    Put32(frame, static_cast<std::uint32_t>(body.size() + 4));
    frame.insert(frame.end(), body.begin(), body.end());
    Send(socket, frame.data(), frame.size());
}
void ErrorMessage(WireSocket &socket, const WireError &error) {
    Buffer body;
    body.push_back('S'); PutString(body, error.fatal ? "FATAL" : "ERROR");
    body.push_back('V'); PutString(body, error.fatal ? "FATAL" : "ERROR");
    body.push_back('C'); PutString(body, error.code);
    body.push_back('M'); PutString(body, error.what());
    body.push_back(0); Message(socket, 'E', body);
}
std::string Upper(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::string FirstWord(const std::string &sql) {
    std::size_t begin = 0;
    for (;;) {
        begin = sql.find_first_not_of(" \t\r\n;", begin);
        if (begin == std::string::npos) return {};
        if (sql.compare(begin, 2, "--") == 0) {
            begin = sql.find('\n', begin + 2);
            if (begin == std::string::npos) return {};
            continue;
        }
        if (sql.compare(begin, 2, "/*") == 0) {
            std::size_t depth = 1; begin += 2;
            while (depth && begin < sql.size()) {
                if (sql.compare(begin, 2, "/*") == 0) { ++depth; begin += 2; }
                else if (sql.compare(begin, 2, "*/") == 0) { --depth; begin += 2; }
                else ++begin;
            }
            if (depth) return {};
            continue;
        }
        break;
    }
    if (begin == std::string::npos) return {};
    auto end = sql.find_first_of(" \t\r\n;(", begin);
    return Upper(sql.substr(begin, end - begin));
}
bool TransactionEnd(const std::string &sql) {
    auto word = FirstWord(sql); return word == "ROLLBACK" || word == "ABORT" || word == "COMMIT" || word == "END";
}
std::string NormalizeTransactions(const std::string &sql) {
    // Use DuckDB's lexer so comments, quoted strings and dollar strings never become protocol controls.
    const auto tokens = duckdb::Parser::Tokenize(sql);
    std::string normalized;
    std::size_t begin = 0, first = 0;
    for (std::size_t boundary = 0; boundary <= tokens.size(); ++boundary) {
        const bool separator = boundary < tokens.size() && tokens[boundary].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && sql[tokens[boundary].start] == ';';
        if (boundary < tokens.size() && !separator) continue;
        const auto end = separator ? tokens[boundary].start : sql.size();
        std::vector<std::string> words; bool plain = true;
        for (std::size_t i = first; i < boundary; ++i) {
            if (tokens[i].start >= sql.size()) continue; // HighlightTokenizer emits an EOF token.
            if (tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) continue;
            auto at = static_cast<std::size_t>(tokens[i].start); auto after = at;
            while (after < sql.size() && (std::isalpha(static_cast<unsigned char>(sql[after])) || sql[after] == '_')) ++after;
            if (after == at) plain = false;
            words.push_back(Upper(sql.substr(at, after - at)));
        }
        bool mapped = false;
        if (!words.empty() && (words[0] == "BEGIN" || words[0] == "START")) {
            std::size_t at = 1;
            if (at < words.size() && (words[at] == "TRANSACTION" || words[at] == "WORK")) ++at;
            else if (words[0] == "START") plain = false;
            if (at < words.size()) {
                if (!plain || words.size() - at < 3 || words[at] != "ISOLATION" || words[at + 1] != "LEVEL")
                    throw WireError("0A000", "Unsupported BEGIN options; DuckDB transactions use snapshot isolation");
                at += 2;
                const bool read = words.size() - at == 2 && words[at] == "READ" && (words[at + 1] == "COMMITTED" || words[at + 1] == "UNCOMMITTED");
                const bool repeatable = words.size() - at == 2 && words[at] == "REPEATABLE" && words[at + 1] == "READ";
                if (!read && !repeatable) throw WireError("0A000", "Only DuckDB snapshot isolation is supported; SERIALIZABLE and other BEGIN options are unavailable");
                normalized += "BEGIN TRANSACTION"; mapped = true;
            }
        }
        if (!mapped) normalized += sql.substr(begin, end - begin);
        if (separator) normalized += ';';
        begin = separator ? end + 1 : end; first = boundary + 1;
    }
    return normalized;
}
std::string ErrorState(const duckdb::ErrorData &error) {
    using E = duckdb::ExceptionType;
    const auto message = Upper(error.RawMessage());
    switch (error.Type()) {
    case E::INTERRUPT: return "57014";
    case E::PARSER: case E::SYNTAX: return "42601";
    case E::CONVERSION: return "22P02";
    case E::OUT_OF_RANGE: case E::DECIMAL: return "22003";
    case E::DIVIDE_BY_ZERO: return "22012";
    case E::NOT_IMPLEMENTED: case E::MISSING_EXTENSION: return "0A000";
    case E::OUT_OF_MEMORY: return "53200";
    case E::PERMISSION: return "42501";
    case E::IO: case E::HTTP: return "58030";
    case E::TRANSACTION:
        return message.find("CONFLICT") != std::string::npos ? "40001" : "25000";
    case E::CATALOG:
        if (message.find("ALREADY EXISTS") != std::string::npos) return "42P07";
        return "42P01";
    case E::BINDER:
        if (message.find("COLUMN") != std::string::npos && message.find("NOT FOUND") != std::string::npos) return "42703";
        return "42804";
    case E::CONSTRAINT:
        if (message.find("NOT NULL") != std::string::npos) return "23502";
        if (message.find("FOREIGN KEY") != std::string::npos) return "23503";
        if (message.find("CHECK") != std::string::npos) return "23514";
        if (message.find("DUPLICATE") != std::string::npos || message.find("UNIQUE") != std::string::npos) return "23505";
        return "23000";
    case E::INVALID_INPUT: return "22023";
    default: return "XX000";
    }
}
[[noreturn]] void NativeError(const duckdb::ErrorData &error) {
    throw WireError(ErrorState(error), error.RawMessage());
}
struct TypeInfo {
    std::uint32_t oid = 25;
    std::int16_t size = -1;
    std::int32_t modifier = -1;
};
TypeInfo PGType(const LogicalType &type) {
    if (type.IsJSONType()) return {114, -1, -1};
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: return {16, 1, -1};
    case LogicalTypeId::TINYINT: case LogicalTypeId::SMALLINT: case LogicalTypeId::UTINYINT: return {21, 2, -1};
    case LogicalTypeId::INTEGER: case LogicalTypeId::USMALLINT: return {23, 4, -1};
    case LogicalTypeId::BIGINT: return {20, 8, -1};
    case LogicalTypeId::UINTEGER: return {26, 4, -1};
    case LogicalTypeId::FLOAT: return {700, 4, -1};
    case LogicalTypeId::DOUBLE: return {701, 8, -1};
    case LogicalTypeId::DECIMAL:
        return {1700, -1, 4 + (duckdb::DecimalType::GetWidth(type) << 16) + duckdb::DecimalType::GetScale(type)};
    case LogicalTypeId::HUGEINT: case LogicalTypeId::UHUGEINT: case LogicalTypeId::UBIGINT: return {1700, -1, -1};
    case LogicalTypeId::BLOB: return {17, -1, -1};
    case LogicalTypeId::DATE: return {1082, 4, -1};
    case LogicalTypeId::TIME: case LogicalTypeId::TIME_NS: return {1083, 8, -1};
    case LogicalTypeId::TIME_TZ: return {1266, 12, -1};
    case LogicalTypeId::TIMESTAMP: case LogicalTypeId::TIMESTAMP_SEC:
    case LogicalTypeId::TIMESTAMP_MS: case LogicalTypeId::TIMESTAMP_NS: return {1114, 8, -1};
    case LogicalTypeId::TIMESTAMP_TZ: case LogicalTypeId::TIMESTAMP_TZ_NS: return {1184, 8, -1};
    case LogicalTypeId::INTERVAL: return {1186, 16, -1};
    case LogicalTypeId::UUID: return {2950, 16, -1};
    default: return {25, -1, -1}; // Complex DuckDB values are explicitly exposed as text.
    }
}
LogicalType DuckType(std::uint32_t oid) {
    switch (oid) {
    case 16: return LogicalType::BOOLEAN;
    case 17: return LogicalType::BLOB;
    case 20: return LogicalType::BIGINT;
    case 21: return LogicalType::SMALLINT;
    case 23: return LogicalType::INTEGER;
    case 26: return LogicalType::UINTEGER;
    case 700: return LogicalType::FLOAT;
    case 701: return LogicalType::DOUBLE;
    case 1082: return LogicalType::DATE;
    case 1083: return LogicalType::TIME;
    case 1114: return LogicalType::TIMESTAMP;
    case 1184: return LogicalType::TIMESTAMP_TZ;
    case 1266: return LogicalType::TIME_TZ;
    case 1186: return LogicalType::INTERVAL;
    case 2950: return LogicalType::UUID;
    case 114: case 3802: return LogicalType::JSON();
    case 0: case 18: case 19: case 25: case 1042: case 1043: case 705: return LogicalType::VARCHAR;
    case 1700: return LogicalType::DECIMAL(38, 0);
    default: throw WireError("0A000", "Unsupported PostgreSQL parameter type OID " + std::to_string(oid));
    }
}
int HexDigit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw WireError("22P02", "Invalid hexadecimal bytea/UUID value");
}
std::string Hex(const std::string &input) {
    static const char digits[] = "0123456789abcdef";
    std::string out; out.reserve(input.size() * 2);
    for (unsigned char c : input) { out.push_back(digits[c >> 4]); out.push_back(digits[c & 15]); }
    return out;
}
std::string DecodeBytea(const std::string &input) {
    std::string out;
    if (input.size() >= 2 && input.substr(0, 2) == "\\x") {
        if ((input.size() - 2) % 2) throw WireError("22P02", "Invalid hexadecimal bytea length");
        for (std::size_t i = 2; i < input.size(); i += 2) out.push_back(static_cast<char>((HexDigit(input[i]) << 4) | HexDigit(input[i + 1])));
        return out;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\') { out.push_back(input[i]); continue; }
        if (++i >= input.size()) throw WireError("22P02", "Invalid bytea escape");
        if (input[i] == '\\') { out.push_back('\\'); continue; }
        if (i + 2 >= input.size() || input[i] < '0' || input[i] > '3' ||
            input[i + 1] < '0' || input[i + 1] > '7' || input[i + 2] < '0' || input[i + 2] > '7')
            throw WireError("22P02", "Invalid bytea escape");
        out.push_back(static_cast<char>((input[i] - '0') * 64 + (input[i + 1] - '0') * 8 + input[i + 2] - '0')); i += 2;
    }
    return out;
}
std::string Four(unsigned value) {
    if (value > 9999) throw WireError("22P03", "Invalid binary numeric digit");
    auto text = std::to_string(value); return std::string(4 - text.size(), '0') + text;
}
Value NumericValue(const std::string &text) {
    // Expand decimal exponents as characters, preserving exact digits without a floating-point round trip.
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) throw WireError("22P02", "Invalid numeric parameter");
    auto input = text.substr(first, last - first + 1);
    std::size_t at = 0; bool negative = false;
    if (input[at] == '+' || input[at] == '-') { negative = input[at] == '-'; ++at; }
    std::string digits; std::size_t before = 0; bool dot = false;
    while (at < input.size()) {
        auto c = input[at];
        if (c >= '0' && c <= '9') { digits.push_back(c); if (!dot) ++before; ++at; }
        else if (c == '.' && !dot) { dot = true; ++at; }
        else break;
    }
    if (digits.empty()) throw WireError("22P02", "Invalid or non-finite numeric parameter");
    std::int64_t exponent = 0;
    if (at < input.size() && (input[at] == 'e' || input[at] == 'E')) {
        ++at; bool exponent_negative = false;
        if (at < input.size() && (input[at] == '+' || input[at] == '-')) { exponent_negative = input[at] == '-'; ++at; }
        auto exponent_begin = at;
        while (at < input.size() && input[at] >= '0' && input[at] <= '9') {
            exponent = exponent * 10 + input[at++] - '0';
            if (exponent > 10000) throw WireError("22003", "Numeric exponent exceeds DuckDB's decimal range");
        }
        if (at == exponent_begin) throw WireError("22P02", "Invalid numeric exponent");
        if (exponent_negative) exponent = -exponent;
    }
    if (at != input.size()) throw WireError("22P02", "Invalid numeric parameter");
    auto point = static_cast<std::int64_t>(before) + exponent;
    auto scale = std::max<std::int64_t>(0, static_cast<std::int64_t>(digits.size()) - point);
    if (scale > 38) throw WireError("22003", "Numeric exceeds DuckDB's 38-digit decimal range");
    auto nonzero = digits.find_first_not_of('0');
    auto integer_digits = nonzero == std::string::npos ? 0 : std::max<std::int64_t>(0, point - static_cast<std::int64_t>(nonzero));
    if (integer_digits + scale > 38) throw WireError("22003", "Numeric exceeds DuckDB's 38-digit decimal range");
    if (nonzero == std::string::npos) return Value("0").DefaultCastAs(LogicalType::DECIMAL(38, static_cast<std::uint8_t>(scale)));
    std::string expanded;
    if (point <= 0) expanded = "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
    else if (point >= static_cast<std::int64_t>(digits.size())) expanded = digits + std::string(static_cast<std::size_t>(point - digits.size()), '0');
    else expanded = digits.substr(0, static_cast<std::size_t>(point)) + "." + digits.substr(static_cast<std::size_t>(point));
    if (negative) expanded.insert(expanded.begin(), '-');
    return Value(expanded).DefaultCastAs(LogicalType::DECIMAL(38, static_cast<std::uint8_t>(scale)));
}
std::string DecodeNumeric(Reader &in) {
    auto digits = in.U16();
    auto weight = static_cast<std::int16_t>(in.U16());
    auto sign = in.U16(); auto scale = in.U16();
    if (digits > 32 || scale > 38 || weight > 10 || weight < -11) throw WireError("22003", "Binary numeric exceeds DuckDB's decimal range");
    if (sign != 0 && sign != 0x4000) throw WireError("0A000", "Non-finite numeric values are unsupported");
    std::vector<unsigned> groups;
    for (unsigned i = 0; i < digits; ++i) { auto value = in.U16(); Four(value); groups.push_back(value); }
    auto group = [&](int position) {
        auto index = int(weight) - position;
        return index >= 0 && index < int(groups.size()) ? groups[index] : 0;
    };
    std::string integer;
    for (int i = std::max<int>(weight, 0); i >= 0; --i) integer += Four(group(i));
    auto first = integer.find_first_not_of('0'); integer = first == std::string::npos ? "0" : integer.substr(first);
    std::string fraction;
    for (unsigned i = 0; i < (scale + 3) / 4; ++i) fraction += Four(group(-1 - int(i)));
    fraction.resize(scale);
    return (sign ? "-" : "") + integer + (scale ? "." + fraction : "");
}
Buffer EncodeNumeric(const std::string &text) {
    bool negative = !text.empty() && text[0] == '-';
    auto value = negative ? text.substr(1) : text;
    auto dot = value.find('.');
    auto integer = value.substr(0, dot);
    auto fraction = dot == std::string::npos ? std::string() : value.substr(dot + 1);
    for (char c : integer + fraction) if (c < '0' || c > '9') throw WireError("0A000", "Unsupported non-finite numeric result");
    auto scale = fraction.size();
    integer.insert(0, (4 - integer.size() % 4) % 4, '0');
    fraction.append((4 - fraction.size() % 4) % 4, '0');
    auto joined = integer + fraction;
    std::vector<std::uint16_t> groups;
    for (std::size_t i = 0; i < joined.size(); i += 4) groups.push_back(static_cast<std::uint16_t>(std::stoi(joined.substr(i, 4))));
    int weight = int(integer.size() / 4) - 1;
    while (!groups.empty() && groups.front() == 0) { groups.erase(groups.begin()); --weight; }
    while (!groups.empty() && groups.back() == 0) groups.pop_back();
    if (groups.empty()) { weight = 0; negative = false; }
    Buffer out; Put16(out, static_cast<std::uint16_t>(groups.size())); Put16(out, static_cast<std::uint16_t>(weight));
    Put16(out, negative ? 0x4000 : 0); Put16(out, static_cast<std::uint16_t>(scale));
    for (auto group : groups) Put16(out, group);
    return out;
}
Value Parameter(std::uint32_t oid, std::uint16_t format, const Buffer &bytes) {
    if (format > 1) throw WireError("08P01", "Parameter format must be text or binary");
    auto target = DuckType(oid);
    if (!format) {
        auto text = std::string(bytes.begin(), bytes.end());
        if (oid == 17) return Value::BLOB_RAW(DecodeBytea(text));
        if (text.find('\0') != std::string::npos) throw WireError("22021", "A text parameter contains a zero byte");
        if (oid == 1700) return NumericValue(text);
        return Value(text).DefaultCastAs(target);
    }
    Reader in(bytes); Value value;
    switch (oid) {
    case 16:
        if (bytes.size() != 1 || bytes[0] > 1) throw WireError("22P03", "Invalid binary boolean");
        value = Value::BOOLEAN(in.Byte() != 0); break;
    case 21: value = Value::SMALLINT(static_cast<std::int16_t>(in.U16())); break;
    case 23: value = Value::INTEGER(static_cast<std::int32_t>(in.U32())); break;
    case 26: value = Value::UINTEGER(in.U32()); break;
    case 20: value = Value::BIGINT(static_cast<std::int64_t>(in.U64())); break;
    case 700: { auto bits = in.U32(); float number; std::memcpy(&number, &bits, 4); value = Value::FLOAT(number); break; }
    case 701: { auto bits = in.U64(); double number; std::memcpy(&number, &bits, 8); value = Value::DOUBLE(number); break; }
    case 17: value = Value::BLOB_RAW(std::string(bytes.begin(), bytes.end())); in.at = bytes.size(); break;
    case 1700: value = NumericValue(DecodeNumeric(in)); break;
    case 1082: {
        auto days = static_cast<std::int32_t>(in.U32());
        if (days == std::numeric_limits<std::int32_t>::max() || days == std::numeric_limits<std::int32_t>::min())
            value = Value(days > 0 ? "infinity" : "-infinity").DefaultCastAs(LogicalType::DATE);
        else {
            auto epoch = std::int64_t(days) + PG_EPOCH_DAYS;
            if (epoch > std::numeric_limits<std::int32_t>::max() || epoch < std::numeric_limits<std::int32_t>::min())
                throw WireError("22008", "Date outside DuckDB range");
            value = Value::DATE(duckdb::date_t(static_cast<std::int32_t>(epoch)));
        }
        break;
    }
    case 1083: value = Value::TIME(duckdb::dtime_t(static_cast<std::int64_t>(in.U64()))); break;
    case 1114: case 1184: {
        auto micros = static_cast<std::int64_t>(in.U64());
        if (micros == std::numeric_limits<std::int64_t>::max() || micros == std::numeric_limits<std::int64_t>::min())
            value = Value(micros > 0 ? "infinity" : "-infinity").DefaultCastAs(target);
        else {
            if (micros > std::numeric_limits<std::int64_t>::max() - PG_EPOCH_MICROS)
                throw WireError("22008", "Timestamp outside DuckDB range");
            value = oid == 1184 ? Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(micros + PG_EPOCH_MICROS))
                                : Value::TIMESTAMP(duckdb::timestamp_t(micros + PG_EPOCH_MICROS));
        }
        break;
    }
    case 1266: {
        auto micros = static_cast<std::int64_t>(in.U64()); auto west = static_cast<std::int32_t>(in.U32());
        value = Value::TIMETZ(duckdb::dtime_tz_t(duckdb::dtime_t(micros), -west)); break;
    }
    case 1186: {
        auto micros = static_cast<std::int64_t>(in.U64()); auto days = static_cast<std::int32_t>(in.U32());
        auto months = static_cast<std::int32_t>(in.U32()); value = Value::INTERVAL(months, days, micros); break;
    }
    case 2950: {
        in.Need(16); auto hex = Hex(std::string(bytes.begin(), bytes.end()));
        value = Value::UUID(hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20));
        in.at = 16; break;
    }
    case 3802:
        if (in.Byte() != 1) throw WireError("0A000", "Unsupported jsonb binary version");
        [[fallthrough]];
    case 0: case 18: case 19: case 25: case 1042: case 1043: case 114: case 705:
        value = Value(std::string(bytes.begin() + in.at, bytes.end())).DefaultCastAs(target); in.at = bytes.size(); break;
    default: throw WireError("0A000", "Unsupported binary parameter type OID " + std::to_string(oid));
    }
    in.End(); return value;
}
Buffer Field(const Value &value, std::uint16_t format) {
    const auto oid = PGType(value.type()).oid;
    if (format > 1) throw WireError("08P01", "Result format must be text or binary");
    if (!format) {
        auto text = oid == 16 ? (value.GetValue<bool>() ? "t" : "f") :
                    oid == 17 ? "\\x" + Hex(duckdb::StringValue::Get(value)) : value.ToString();
        return Buffer(text.begin(), text.end());
    }
    Buffer out;
    switch (oid) {
    case 16: out.push_back(value.GetValue<bool>() ? 1 : 0); break;
    case 21: Put16(out, static_cast<std::uint16_t>(value.GetValue<std::int16_t>())); break;
    case 23: Put32(out, static_cast<std::uint32_t>(value.GetValue<std::int32_t>())); break;
    case 26: Put32(out, value.GetValue<std::uint32_t>()); break;
    case 20: Put64(out, static_cast<std::uint64_t>(value.GetValue<std::int64_t>())); break;
    case 700: { float number = value.GetValue<float>(); std::uint32_t bits; std::memcpy(&bits, &number, 4); Put32(out, bits); break; }
    case 701: { double number = value.GetValue<double>(); std::uint64_t bits; std::memcpy(&bits, &number, 8); Put64(out, bits); break; }
    case 1700: return EncodeNumeric(value.ToString());
    case 17: { auto &raw = duckdb::StringValue::Get(value); return Buffer(raw.begin(), raw.end()); }
    case 1082: {
        auto date = value.GetValue<duckdb::date_t>();
        if (!date.IsFinite()) Put32(out, date.days > 0 ? INT32_MAX : static_cast<std::uint32_t>(INT32_MIN));
        else Put32(out, static_cast<std::uint32_t>(std::int64_t(date.days) - PG_EPOCH_DAYS));
        break;
    }
    case 1083: Put64(out, static_cast<std::uint64_t>(value.DefaultCastAs(LogicalType::TIME).GetValue<duckdb::dtime_t>().value)); break;
    case 1114: case 1184: {
        auto micros = oid == 1184 ? value.DefaultCastAs(LogicalType::TIMESTAMP_TZ).GetValue<duckdb::timestamp_tz_t>().value
                                  : value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<duckdb::timestamp_t>().value;
        if (micros == INT64_MAX || micros == INT64_MIN) Put64(out, static_cast<std::uint64_t>(micros));
        else {
            if (micros < INT64_MIN + PG_EPOCH_MICROS) throw WireError("22008", "Timestamp outside PostgreSQL binary range");
            Put64(out, static_cast<std::uint64_t>(micros - PG_EPOCH_MICROS));
        }
        break;
    }
    case 1266: {
        auto time = value.GetValue<duckdb::dtime_tz_t>();
        Put64(out, static_cast<std::uint64_t>(time.time().value)); Put32(out, static_cast<std::uint32_t>(-time.offset())); break;
    }
    case 1186: {
        auto interval = value.GetValue<duckdb::interval_t>();
        Put64(out, static_cast<std::uint64_t>(interval.micros)); Put32(out, static_cast<std::uint32_t>(interval.days)); Put32(out, static_cast<std::uint32_t>(interval.months)); break;
    }
    case 2950: {
        auto text = value.ToString();
        text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
        if (text.size() != 32) throw WireError("XX000", "Invalid native UUID result");
        for (std::size_t i = 0; i < text.size(); i += 2) out.push_back(static_cast<unsigned char>((HexDigit(text[i]) << 4) | HexDigit(text[i + 1])));
        break;
    }
    default: { auto text = value.ToString(); out.assign(text.begin(), text.end()); break; }
    }
    return out;
}
std::uint16_t Format(const std::vector<std::uint16_t> &formats, std::size_t column, std::size_t count) {
    if (formats.empty()) return 0;
    if (formats.size() == 1) return formats[0];
    if (formats.size() != count) throw WireError("08P01", "Format-code count does not match field count");
    return formats[column];
}
bool SessionSetting(const std::string &sql) {
    const auto upper = Upper(sql);
    std::size_t at = 0;
    auto word = [&]() {
        while (at < upper.size() && std::isspace(static_cast<unsigned char>(upper[at]))) ++at;
        std::string value;
        if (at < upper.size() && upper[at] == '"') {
            ++at;
            while (at < upper.size() && upper[at] != '"') value.push_back(upper[at++]);
            if (at < upper.size()) ++at;
        } else {
            while (at < upper.size() && (std::isalnum(static_cast<unsigned char>(upper[at])) || upper[at] == '_')) value.push_back(upper[at++]);
        }
        return value;
    };
    auto command = word(); auto name = word();
    if (command != "SET") return false;
    if (name == "SESSION" || name == "LOCAL" || name == "GLOBAL") name = word();
    const std::array<const char *, 6> supported {{"EXTRA_FLOAT_DIGITS", "APPLICATION_NAME", "CLIENT_ENCODING",
                                               "STANDARD_CONFORMING_STRINGS", "DATESTYLE", "BYTEA_OUTPUT"}};
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) return false;
    if (name == "CLIENT_ENCODING" && Upper(sql).find("UTF8") == std::string::npos && Upper(sql).find("UTF-8") == std::string::npos)
        throw WireError("0A000", "PGwire supports UTF8 client encoding only");
    if (name == "STANDARD_CONFORMING_STRINGS" && Upper(sql).find("OFF") != std::string::npos)
        throw WireError("0A000", "Nonstandard string escaping is unsupported");
    if (name == "BYTEA_OUTPUT" && Upper(sql).find("ESCAPE") != std::string::npos)
        throw WireError("0A000", "PGwire bytea output is hexadecimal");
    if (name == "DATESTYLE" && Upper(sql).find("ISO") == std::string::npos)
        throw WireError("0A000", "PGwire supports ISO DateStyle only");
    return true;
}
bool DeallocateAll(const std::string &sql) {
    auto normalized = Upper(sql);
    auto end = normalized.find_last_not_of(" \t\r\n;");
    if (end == std::string::npos) return false;
    normalized.resize(end + 1);
    std::istringstream input(normalized); std::string command, target, extra;
    input >> command >> target;
    if (command != "DEALLOCATE") return false;
    if (target == "PREPARE") input >> target;
    return target == "ALL" && !(input >> extra);
}

std::string CursorName(const std::string &value) {
    std::string name;
    if (!value.empty() && value.front() == '"') {
        for (std::size_t i = 1; i + 1 < value.size(); ++i) {
            name.push_back(value[i]);
            if (value[i] == '"') ++i;
        }
    } else {
        name = value;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    if (name.empty() || name.size() > 63) throw WireError("42602", "Cursor or savepoint identifier must contain 1..63 bytes");
    return name;
}
const std::string SQL_IDENTIFIER = R"(("(?:[^"]|"")+"|[A-Za-z_][A-Za-z0-9_]*))";
enum class FdwKind { NONE, SETTING, FETCH, CLOSE, CLOSE_ALL, SAVEPOINT, ROLLBACK_TO, RELEASE };
struct FdwCommand { FdwKind kind = FdwKind::NONE; std::string name; std::uint64_t amount = 0; };
struct FdwDeclaration { std::string name, query; };
bool FdwDeclare(const std::string &sql, FdwDeclaration &command) {
    if (FirstWord(sql) != "DECLARE") return false;
    static const std::regex pattern("^\\s*DECLARE\\s+" + SQL_IDENTIFIER + R"(\s+(?:NO\s+SCROLL\s+)?CURSOR\s+FOR\s+([\s\S]+)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(sql, match, pattern)) throw WireError("0A000", "SQL cursors support forward non-hold SELECT only");
    command.name = CursorName(match[1]); command.query = match[2]; return true;
}
bool FdwSetting(const std::string &sql) {
    static const std::array<const char *,3> commands{{"SET SEARCH_PATH = PG_CATALOG", "SET TIMEZONE = 'GMT'", "SET INTERVALSTYLE = POSTGRES"}};
    auto value = Upper(sql); while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) || value.back()==';')) value.pop_back();
    auto first=value.find_first_not_of(" \t\r\n"); if(first!=std::string::npos)value.erase(0,first);
    return std::find(commands.begin(), commands.end(), value) != commands.end();
}
FdwCommand FdwControl(const std::string &sql) {
    FdwCommand command;
    if (FdwSetting(sql)) { command.kind = FdwKind::SETTING; return command; }
    const auto word = FirstWord(sql);
    std::smatch match;
    if (word == "FETCH") {
        static const std::regex pattern("^\\s*FETCH\\s+(?:FORWARD\\s+)?([0-9]+|ALL)\\s+(?:FROM|IN)\\s+" + SQL_IDENTIFIER + R"(\s*;?\s*$)", std::regex::icase);
        if (!std::regex_match(sql, match, pattern)) throw WireError("0A000", "SQL cursors support forward FETCH only");
        command.kind = FdwKind::FETCH; command.name = CursorName(match[2]);
        try { command.amount = Upper(match[1]) == "ALL" ? UINT64_MAX : std::stoull(match[1]); }
        catch (const std::exception &) { throw WireError("22003", "FETCH count is out of range"); }
    } else if (word == "CLOSE") {
        static const std::regex all(R"(^\s*CLOSE\s+ALL\s*;?\s*$)", std::regex::icase);
        static const std::regex pattern("^\\s*CLOSE\\s+" + SQL_IDENTIFIER + R"(\s*;?\s*$)", std::regex::icase);
        if (std::regex_match(sql, all)) command.kind = FdwKind::CLOSE_ALL;
        else if (std::regex_match(sql, match, pattern)) { command.kind = FdwKind::CLOSE; command.name = CursorName(match[1]); }
        else throw WireError("0A000", "Unsupported SQL CLOSE command");
    } else if (word == "SAVEPOINT" || word == "RELEASE" || (word == "ROLLBACK" && Upper(sql).find("TO") != std::string::npos)) {
        const std::string prefix = word == "SAVEPOINT" ? "SAVEPOINT" : word == "RELEASE" ? "RELEASE(?:\\s+SAVEPOINT)?" : "ROLLBACK(?:\\s+WORK|\\s+TRANSACTION)?\\s+TO(?:\\s+SAVEPOINT)?";
        const std::regex pattern("^\\s*" + prefix + "\\s+" + SQL_IDENTIFIER + R"(\s*;?\s*$)", std::regex::icase);
        if (!std::regex_match(sql, match, pattern)) throw WireError("42601", "Invalid savepoint command");
        command.kind = word == "SAVEPOINT" ? FdwKind::SAVEPOINT : word == "RELEASE" ? FdwKind::RELEASE : FdwKind::ROLLBACK_TO;
        command.name = CursorName(match[1]);
    }
    return command;
}
std::vector<std::string> SplitCommands(const std::string &sql) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (const auto &token : duckdb::Parser::Tokenize(sql)) {
        if (token.type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && sql[token.start] == ';') {
            if (sql.substr(start, token.start - start).find_first_not_of(" \t\r\n") != std::string::npos)
                result.push_back(sql.substr(start, token.start - start));
            start = token.start + 1;
        }
    }
    if (sql.substr(start).find_first_not_of(" \t\r\n") != std::string::npos) result.push_back(sql.substr(start));
    return result;
}

bool FilenameReference(const duckdb::Identifier &identifier) {
    if (identifier.empty()) return false;
    const auto name = Upper(identifier.GetIdentifierName());
    if (name.find_first_of("/\\:*?[]") != std::string::npos) return true;
    const std::array<const char *, 7> extensions {{".CSV", ".TSV", ".PARQUET", ".JSON", ".JSONL", ".NDJSON", ".DUCKDB"}};
    for (const auto extension : extensions) {
        const auto at = name.rfind(extension);
        if (at == std::string::npos) continue;
        const auto after = name.substr(at + std::strlen(extension));
        if (after.empty() || after == ".GZ" || after == ".ZST" || after == ".ZSTD") return true;
    }
    return false;
}
void GuardFileReferences(duckdb::SQLStatement &statement) {
    std::function<void(duckdb::QueryNode &)> query;
    std::function<void(duckdb::ParsedExpression &)> expression;
    std::function<void(duckdb::TableRef &)> reference;
    expression = [&](duckdb::ParsedExpression &expr) {
        if (expr.GetExpressionClass() == duckdb::ExpressionClass::SUBQUERY) {
            auto &subquery = expr.Cast<duckdb::SubqueryExpression>().SubqueryMutable();
            if (subquery) query(*subquery->node);
        }
        duckdb::ParsedExpressionIterator::EnumerateChildren(expr, [&](duckdb::ParsedExpression &child) { expression(child); });
    };
    const auto name_guard = [&](const duckdb::QualifiedName &name) {
        if (FilenameReference(name.Catalog()) || FilenameReference(name.Schema()) || FilenameReference(name.Name()))
            throw WireError("42501", "File references and replacement scans are not allowed on the published database");
    };
    reference = [&](duckdb::TableRef &ref) {
        if (ref.type == duckdb::TableReferenceType::BASE_TABLE) name_guard(ref.Cast<duckdb::BaseTableRef>().GetQualifiedName());
        else if (ref.type == duckdb::TableReferenceType::TABLE_FUNCTION) {
            auto &subquery = ref.Cast<duckdb::TableFunctionRef>().subquery;
            if (subquery) query(*subquery->node);
        } else if (ref.type == duckdb::TableReferenceType::SHOW_REF) {
            auto &show = ref.Cast<duckdb::ShowRef>(); name_guard(show.qualified_name);
            if (show.query) query(*show.query);
        } else if (ref.type == duckdb::TableReferenceType::PIVOT) {
            for (auto &pivot : ref.Cast<duckdb::PivotRef>().pivots) {
                if (pivot.subquery) query(*pivot.subquery);
                for (auto &expr : pivot.pivot_expressions) if (expr) expression(*expr);
                for (auto &entry : pivot.entries) if (entry.expr) expression(*entry.expr);
            }
        } else if (ref.type == duckdb::TableReferenceType::MATCH_RECOGNIZE) {
            auto &input = ref.Cast<duckdb::MatchRecognizeRef>().input;
            if (input) duckdb::ParsedExpressionIterator::EnumerateTableRefChildren(*input,
                [&](duckdb::unique_ptr<duckdb::ParsedExpression> &expr) { if (expr) expression(*expr); }, reference);
        }
    };
    query = [&](duckdb::QueryNode &node) {
        duckdb::ParsedExpressionIterator::EnumerateQueryNodeChildren(node,
            [&](duckdb::unique_ptr<duckdb::ParsedExpression> &expr) { if (expr) expression(*expr); }, reference);
    };
    using S = duckdb::StatementType;
    switch (statement.type) {
    case S::SELECT_STATEMENT: query(*statement.Cast<duckdb::SelectStatement>().node); break;
    case S::INSERT_STATEMENT: query(*statement.Cast<duckdb::InsertStatement>().node); break;
    case S::UPDATE_STATEMENT: query(*statement.Cast<duckdb::UpdateStatement>().node); break;
    case S::DELETE_STATEMENT: query(*statement.Cast<duckdb::DeleteStatement>().node); break;
    case S::MERGE_INTO_STATEMENT: query(*statement.Cast<duckdb::MergeIntoStatement>().node); break;
    case S::CREATE_STATEMENT: {
        auto &info = *statement.Cast<duckdb::CreateStatement>().info;
        if (info.type == duckdb::CatalogType::TABLE_ENTRY) {
            auto &select = info.Cast<duckdb::CreateTableInfo>().query; if (select) query(*select->node);
        } else if (info.type == duckdb::CatalogType::VIEW_ENTRY) {
            auto &select = info.Cast<duckdb::CreateViewInfo>().query; if (select) query(*select->node);
        }
        break;
    }
    default: break;
    }
}
std::vector<std::string> ControlCommands(const std::string &sql) {
    const auto tokens = duckdb::Parser::Tokenize(sql);
    std::vector<std::string> commands; std::string command;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].start >= sql.size() || tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) continue;
        const auto end = i + 1 < tokens.size() ? tokens[i + 1].start : sql.size();
        auto token = sql.substr(tokens[i].start, end - tokens[i].start);
        const auto last = token.find_last_not_of(" \t\r\n"); if (last == std::string::npos) continue;
        token.resize(last + 1);
        if (token == ";") { if (!command.empty()) commands.push_back(command); command.clear(); }
        else { if (!command.empty()) command += ' '; command += Upper(token); }
    }
    if (!command.empty()) commands.push_back(command);
    return commands;
}
} // namespace

struct PgWireServer::Impl {
    struct Session;
    struct Job { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    duckdb::DuckDB &database;
    std::string host, password;
    bool protected_mode = false;
    PgWireOptions options;
    SSL_CTX *tls_context = nullptr;
    std::string fake_verifier;
    std::uint16_t requested_port;
    std::size_t max_active_queries = 8;
    std::uint32_t query_timeout_ms = 30000;
    std::atomic<std::size_t> active_queries {0};
    // Parsing uses allocations outside the buffer manager. Serialize protected
    // parses, including different SQL, rather than multiplying that working set.
    std::mutex parse_lock;
    std::condition_variable parse_wake;
    bool parse_busy = false;
    struct InsertTemplate {
        std::string sql;
        duckdb::ParserOptions options;
        duckdb::unique_ptr<duckdb::SQLStatement> statement;
    };
    std::unique_ptr<InsertTemplate> insert_template;
    struct SelectTemplate : InsertTemplate {
        std::size_t nodes = 0, strings = 0;
        std::uint64_t touched = 0;
    };
    std::array<std::unique_ptr<SelectTemplate>,16> select_templates;
    std::size_t select_nodes = 0, select_strings = 0;
    std::uint64_t select_clock = 0;
    // All template access is under the existing owner-wide parse admission.
    void PublishSelect(std::unique_ptr<SelectTemplate> next) {
        if (next->nodes > 4096 || next->strings > 128*1024) return;
        std::size_t slot = select_templates.size();
        while (slot == select_templates.size()) {
            for (std::size_t i=0;i<select_templates.size();++i)
                if (!select_templates[i] && select_nodes+next->nodes<=4096 && select_strings+next->strings<=128*1024) { slot=i; break; }
            if (slot != select_templates.size()) break;
            std::size_t oldest = select_templates.size();
            for (std::size_t i=0;i<select_templates.size();++i)
                if (select_templates[i] && (oldest==select_templates.size() || select_templates[i]->touched<select_templates[oldest]->touched)) oldest=i;
            if (oldest == select_templates.size()) return;
            select_nodes-=select_templates[oldest]->nodes; select_strings-=select_templates[oldest]->strings;
            select_templates[oldest].reset();
        }
        // Every potentially throwing allocation happened before publication.
        next->touched=++select_clock;
        select_nodes+=next->nodes; select_strings+=next->strings;
        select_templates[slot]=std::move(next);
    }
    std::atomic<std::uint16_t> bound_port {0};
    std::atomic<bool> stopping {true};
    int listener = -1;
    std::thread accept_thread;
    std::vector<Job> jobs;
    std::mutex sessions_lock, lifecycle_lock;
    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions;
    std::atomic<std::uint32_t> next_id {1};
    Impl(duckdb::DuckDB &db, std::string bind, std::uint16_t port, std::string credential,
         std::size_t maximum, std::uint32_t timeout)
        : database(db), host(std::move(bind)), password(std::move(credential)), requested_port(port),
          max_active_queries(maximum), query_timeout_ms(timeout) {}
    Impl(duckdb::DuckDB &db, PgWireOptions config)
        : database(db), host(config.host), protected_mode(true), options(std::move(config)), requested_port(options.port),
          max_active_queries(options.max_active_queries), query_timeout_ms(options.query_timeout_ms) {}
    ~Impl() { Stop(); if (tls_context) SSL_CTX_free(tls_context); }
    void Start();
    void Stop() noexcept;
    void Accept();
    void Cancel(std::uint32_t id, std::uint32_t secret);
};

struct PgWireServer::Impl::Session {
    struct Statement {
        std::string sql;
        std::string fdw_cursor_name;
        FdwCommand control;
        duckdb::StatementType type = duckdb::StatementType::INVALID_STATEMENT;
        duckdb::unique_ptr<duckdb::PreparedStatement> prepared;
        std::vector<std::uint32_t> parameter_oids;
        std::vector<std::uint32_t> described_oids;
        bool described = false, returns_rows = false;
        bool setting = false, empty = false, deallocate = false;
    };
    struct Portal {
        std::shared_ptr<Statement> statement;
        duckdb::vector<Value> parameters;
        std::vector<std::uint16_t> result_formats;
        duckdb::unique_ptr<duckdb::QueryResult> result;
        duckdb::unique_ptr<duckdb::StatementSignature> signature;
        duckdb::unique_ptr<duckdb::DataChunk> chunk;
        std::size_t chunk_row = 0;
        std::uint64_t sent = 0, changed = 0;
        std::uint64_t generation = 0;
        bool complete = false, described = false, failed = false;
        std::vector<std::uint32_t> described_oids;
        std::string tag;
    };
    Impl &server;
    duckdb::Connection connection;
    std::mutex connection_lock;
    WireSocket socket;
    std::mutex socket_lock;
    std::string authenticated_user;
    bool read_only = true;
    std::uint32_t id, cancel_secret;
    bool failed_transaction = false, skip_until_sync = false, implicit_extended_transaction = false;
    bool implicit_simple_transaction = false;
    bool recoverable_failure = true;
    unsigned suppress_ready = 0;
    std::uint64_t resource_generation = 0;
    std::atomic<std::uint64_t> cancel_generation {0};
    struct Savepoint { std::string name; std::uint64_t transaction_id, generation; };
    std::vector<Savepoint> savepoints;
    std::unordered_map<std::string, std::shared_ptr<Statement>> statements;
    std::unordered_map<std::string, Portal> portals;
    std::unordered_map<std::string, Portal> sql_cursors;
    Session(Impl &owner, int socket_p, std::uint32_t id_p)
        : server(owner), connection(owner.database), socket(socket_p), id(id_p), cancel_secret(0) {
        if (RAND_bytes(reinterpret_cast<unsigned char *>(&cancel_secret), sizeof(cancel_secret)) != 1)
            throw std::runtime_error("Cannot generate cancellation key");
    }
    ~Session() { CloseSocket(); }
    struct ExecutionGuard {
        Session &session;
        std::mutex lock;
        std::condition_variable wake;
        bool finished = false;
        std::thread timer;
        explicit ExecutionGuard(Session &owner) : session(owner) {
            if (session.server.active_queries.fetch_add(1) >= session.server.max_active_queries) {
                session.server.active_queries.fetch_sub(1);
                throw WireError("53300", "The native query capacity is full; retry after an active query completes");
            }
        }
        void StartTimer() {
            timer = std::thread([this] {
                std::unique_lock<std::mutex> guard(lock);
                if (wake.wait_for(guard, std::chrono::milliseconds(session.server.query_timeout_ms), [this] { return finished; })) return;
                while (!finished) {
                    guard.unlock(); try { session.Interrupt(); } catch (...) {} guard.lock();
                    // Native statement startup clears prior interrupt flags. Reissue
                    // after the deadline until this execution completes, then join
                    // before any later statement can start on the same connection.
                    wake.wait_for(guard, std::chrono::milliseconds(10), [this] { return finished; });
                }
            });
        }
        ~ExecutionGuard() {
            { std::lock_guard<std::mutex> guard(lock); finished = true; }
            wake.notify_one();
            if (timer.joinable()) timer.join();
            session.server.active_queries.fetch_sub(1);
        }
        ExecutionGuard(const ExecutionGuard &) = delete;
        ExecutionGuard &operator=(const ExecutionGuard &) = delete;
    };
    struct ParseAdmission {
        Session &session;
        std::uint64_t generation;
        std::chrono::steady_clock::time_point deadline;
        explicit ParseAdmission(Session &owner) : session(owner), generation(owner.cancel_generation.load()) {
            std::unique_lock<std::mutex> lock(session.server.parse_lock);
            deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::min<std::uint32_t>(30000, session.server.query_timeout_ms));
            if (!session.server.parse_wake.wait_until(lock, deadline, [&] {
                return !session.server.parse_busy || session.server.stopping.load() ||
                       session.cancel_generation.load() != generation;
            })) throw WireError("53300", "Native parse capacity is full; retry after parsing completes");
            Check();
            session.server.parse_busy = true;
        }
        void Check() const {
            if (session.server.stopping.load()) throw WireError("57P01", "Server is stopping", true);
            if (session.cancel_generation.load() != generation)
                throw WireError("57014", "Statement cancelled during native parsing");
            if (std::chrono::steady_clock::now() >= deadline)
                throw WireError("57014", "Native parsing exceeded the statement deadline");
        }
        ~ParseAdmission() {
            { std::lock_guard<std::mutex> lock(session.server.parse_lock); session.server.parse_busy = false; }
            session.server.parse_wake.notify_all();
        }
        ParseAdmission(const ParseAdmission &) = delete;
        ParseAdmission &operator=(const ParseAdmission &) = delete;
    };
    void Shutdown() {
        Interrupt();
        std::lock_guard<std::mutex> guard(socket_lock);
        if (socket >= 0) shutdown(socket, SHUT_RDWR);
    }
    void Interrupt() {
        cancel_generation.fetch_add(1);
        server.parse_wake.notify_all();
        duckdb::shared_ptr<duckdb::ClientContext> context;
        { std::lock_guard<std::mutex> guard(connection_lock); context = connection.context; }
        if (context) context->Interrupt();
    }
    void CloseSocket() {
        std::lock_guard<std::mutex> guard(socket_lock);
        if (socket.tls) { SSL_free(socket.tls); socket.tls = nullptr; }
        if (socket >= 0) { close(socket); socket = -1; }
    }
    char State() { return connection.IsAutoCommit() ? 'I' : failed_transaction ? 'E' : 'T'; }
    void Ready() {
        if (connection.IsAutoCommit()) TransactionFinished();
        if (!suppress_ready) Message(socket, 'Z', Buffer {static_cast<unsigned char>(State())});
    }
    void Run();
    bool Startup();
    void UpgradeTLS();
    void Authenticate(const std::map<std::string, std::string> &options);
    Buffer AuthenticationResponse();
    void Authorize(duckdb::SQLStatement &statement);
    void AuthorizeBound(duckdb::StatementProperties properties);
    void InitializeTypeCatalog();
    void ResetConnection(bool preserve_statements);
    void EnsurePrepared(Statement &statement);
    void Handle(char type, const Buffer &body);
    void Simple(const std::string &sql);
    bool FdwSimple(const std::string &sql);
    void FdwStore(Portal &&portal);
    void FdwExecute(Portal &portal, std::uint32_t maximum, bool simple);
    void ReadOnlyTransaction();
    void SavepointControl(const FdwCommand &command);
    void RecordFailure(const std::string &state, bool fatal = false);
    void TransactionFinished() {
        failed_transaction = false; recoverable_failure = true;
        sql_cursors.clear(); savepoints.clear();
    }
    void Parse(Reader &in);
    duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> Extract(const std::string &sql,
                                                                 ParseAdmission *admission = nullptr);
    void Bind(Reader &in);
    duckdb::StatementSignature BoundSignature(const Statement &statement, const duckdb::vector<Value> &parameters);
    void Describe(Reader &in);
    void Execute(Reader &in);
    void Rows(Portal &portal, std::uint32_t maximum);
    void RowDescription(const duckdb::vector<duckdb::Identifier> &names, const duckdb::vector<LogicalType> &types,
                        const std::vector<std::uint16_t> &formats, Portal *portal = nullptr) {
        if (types.size() > 32767) throw WireError("54000", "Too many result columns");
        Buffer body; Put16(body, static_cast<std::uint16_t>(types.size()));
        if (portal) portal->described_oids.clear();
        for (std::size_t i = 0; i < types.size(); ++i) {
            auto info = PGType(types[i]); auto format = Format(formats, i, types.size());
            PutString(body, names[i].GetIdentifierName()); Put32(body, 0); Put16(body, 0);
            Put32(body, info.oid); Put16(body, static_cast<std::uint16_t>(info.size)); Put32(body, static_cast<std::uint32_t>(info.modifier)); Put16(body, format);
            if (portal) portal->described_oids.push_back(info.oid);
        }
        Message(socket, 'T', body);
        if (portal) portal->described = true;
    }
    void GuardTransaction(const std::string &sql) {
        const auto control = FdwControl(sql);
        if (failed_transaction && !TransactionEnd(sql) && control.kind != FdwKind::ROLLBACK_TO)
            throw WireError("25P02", "Current transaction is aborted; issue ROLLBACK or ROLLBACK TO SAVEPOINT");
    }
    std::string CommandTag(const Statement &statement, const duckdb::QueryResult &result, std::uint64_t rows) {
        using S = duckdb::StatementType;
        const auto &sql = statement.sql;
        const auto type = statement.type == S::INVALID_STATEMENT ? result.GetStatementType() : statement.type;
        switch (type) {
        case S::INSERT_STATEMENT: return "INSERT 0 " + std::to_string(rows);
        case S::UPDATE_STATEMENT: return "UPDATE " + std::to_string(rows);
        case S::DELETE_STATEMENT: return "DELETE " + std::to_string(rows);
        case S::MERGE_INTO_STATEMENT: return "MERGE " + std::to_string(rows);
        case S::SELECT_STATEMENT: case S::EXPLAIN_STATEMENT: return "SELECT " + std::to_string(rows);
        case S::CREATE_STATEMENT: return "CREATE " + (Upper(sql).find("VIEW") != std::string::npos ? std::string("VIEW") : std::string("TABLE"));
        case S::DROP_STATEMENT: return "DROP " + (Upper(sql).find("VIEW") != std::string::npos ? std::string("VIEW") : std::string("TABLE"));
        case S::ALTER_STATEMENT: return "ALTER TABLE";
        case S::TRANSACTION_STATEMENT: {
            auto word = FirstWord(sql);
            return word == "END" ? "COMMIT" : word == "START" ? "BEGIN" : word == "ABORT" ? "ROLLBACK" : word;
        }
        case S::COPY_STATEMENT: return "COPY " + std::to_string(rows);
        default: return FirstWord(sql);
        }
    }
};

void PgWireServer::Impl::Session::AuthorizeBound(duckdb::StatementProperties properties) {
    if (server.protected_mode && read_only && !properties.IsReadOnly())
        throw WireError("42501", "This database user has read-only access");
}
void PgWireServer::Impl::Session::Authorize(duckdb::SQLStatement &statement) {
    if (!server.protected_mode) return;
    using S = duckdb::StatementType;
    const auto denied = [] { throw WireError("42501", "This SQL operation is not allowed on the published database"); };
    if (statement.type == S::EXPLAIN_STATEMENT) {
        auto &explain = statement.Cast<duckdb::ExplainStatement>();
        Authorize(*explain.stmt); // EXPLAIN ANALYZE must never hide a prohibited operation.
    } else if (statement.type == S::SELECT_STATEMENT || statement.type == S::TRANSACTION_STATEMENT) {
        // The bound plan below additionally rejects SELECT expressions that modify the database.
    } else {
        if (read_only) denied();
        switch (statement.type) {
        case S::INSERT_STATEMENT: case S::UPDATE_STATEMENT: case S::DELETE_STATEMENT:
        case S::MERGE_INTO_STATEMENT: case S::ALTER_STATEMENT: break;
        case S::CREATE_STATEMENT: case S::DROP_STATEMENT: {
            const auto type = statement.type == S::CREATE_STATEMENT ? statement.Cast<duckdb::CreateStatement>().info->type
                                                                   : statement.Cast<duckdb::DropStatement>().info->type;
            using C = duckdb::CatalogType;
            if (type != C::TABLE_ENTRY && type != C::VIEW_ENTRY && type != C::INDEX_ENTRY && type != C::SCHEMA_ENTRY &&
                type != C::SEQUENCE_ENTRY && type != C::TYPE_ENTRY) denied();
            break;
        }
        default: denied();
        }
    }
    GuardFileReferences(statement);
    // Native statement structure governs the allowlist. The native lexer adds a
    // conservative ban on indirect SQL and Quack control functions, including
    // quoted/schema-qualified names. Strings and comments cannot trigger it.
    const auto sql = statement.ToString(); const auto tokens = duckdb::Parser::Tokenize(sql);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].start >= sql.size() || tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT ||
            tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT) continue;
        auto end = i + 1 < tokens.size() ? tokens[i + 1].start : sql.size();
        auto token = sql.substr(tokens[i].start, end - tokens[i].start);
        const auto last = token.find_last_not_of(" \t\r\n"); if (last == std::string::npos) continue;
        token.resize(last + 1); token = Upper(token);
        if (token.size() > 1 && token.front() == '"' && token.back() == '"') token = token.substr(1, token.size() - 2);
        std::size_t next = i + 1;
        while (next < tokens.size() && tokens[next].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) ++next;
        const bool call = next < tokens.size() && tokens[next].start < sql.size() && sql[tokens[next].start] == '(';
        if (token == "QUERY" || token == "QUERY_TABLE" || token.compare(0, 6, "QUACK_") == 0 || token == "RPC_AUTHORIZATION" ||
            (call && (token == "SET_CONFIG" || token == "SET_VARIABLE" || token.compare(0, 5, "READ_") == 0 ||
                      token == "PARQUET_SCAN" || token == "CSV_SCAN" || token == "JSON_SCAN" || token == "GLOB" || token == "LIST_FILES"))) denied();
    }
}
void PgWireServer::Impl::Session::UpgradeTLS() {
    if (socket.tls || !server.tls_context) throw WireError("08P01", "Invalid TLS negotiation", true);
    socket.tls = SSL_new(server.tls_context);
    if (!socket.tls || SSL_set_fd(socket.tls, socket.fd) != 1) throw WireError("08006", "TLS initialization failed", true);
    for (;;) {
        const int result = SSL_accept(socket.tls);
        if (result == 1) return;
        const auto error = SSL_get_error(socket.tls, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) throw Disconnected();
        if (!WaitSocket(socket.fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, socket.read_deadline)) throw Disconnected();
    }
}
Buffer PgWireServer::Impl::Session::AuthenticationResponse() {
    unsigned char type; std::array<unsigned char, 4> header {};
    if (!Receive(socket, &type, 1) || type != 'p' || !Receive(socket, header.data(), header.size())) throw Disconnected();
    Buffer bytes(header.begin(), header.end()); Reader reader(bytes); const auto size = reader.U32();
    if (size < 4 || size > 4100) throw WireError("28P01", "Authentication failed", true);
    Buffer response(size - 4);
    if (!response.empty() && !Receive(socket, response.data(), response.size())) throw Disconnected();
    return response;
}
void PgWireServer::Impl::Session::Authenticate(const std::map<std::string, std::string> &startup) {
    const auto user = startup.find("user"), database = startup.find("database");
    const PgUser *selected = nullptr;
    if (user != startup.end() && database != startup.end() && database->second == server.options.database_name)
        for (const auto &candidate : server.options.users) if (candidate.name == user->second) { selected = &candidate; break; }
    const auto identity = (user == startup.end() ? std::string() : user->second) + '\0' +
                          (database == startup.end() ? std::string() : database->second);
    const auto verifier = selected ? selected->scram_verifier : MockScramVerifier(server.fake_verifier, identity);
    Buffer body; Put32(body, 10); PutString(body, "SCRAM-SHA-256"); body.push_back(0); Message(socket, 'R', body);
    try {
        ScramExchange exchange(verifier);
        auto initial = AuthenticationResponse(); Reader first(initial);
        if (first.String() != "SCRAM-SHA-256") throw std::invalid_argument("Unsupported SASL mechanism");
        const auto count = first.U32();
        if (count > 4096) throw std::invalid_argument("Invalid SASL initial response");
        const auto bytes = first.Bytes(count); first.End();
        const auto challenge = exchange.Challenge(std::string(bytes.begin(), bytes.end()));
        body.clear(); Put32(body, 11); body.insert(body.end(), challenge.begin(), challenge.end()); Message(socket, 'R', body);
        const auto final = AuthenticationResponse();
        const auto response = exchange.Finish(std::string(final.begin(), final.end()));
        if (!selected) throw std::invalid_argument("Unknown credentials");
        authenticated_user = selected->name; read_only = selected->read_only;
        body.clear(); Put32(body, 12); body.insert(body.end(), response.begin(), response.end()); Message(socket, 'R', body);
    } catch (const std::invalid_argument &) { throw WireError("28P01", "Authentication failed", true); }
}
bool PgWireServer::Impl::Session::Startup() {
    for (unsigned tries = 0; tries < 3; ++tries) {
        std::array<unsigned char, 4> length_bytes {};
        if (!Receive(socket, length_bytes.data(), length_bytes.size())) return false;
        Buffer length_buffer(length_bytes.begin(), length_bytes.end()); Reader length_reader(length_buffer);
        auto length = length_reader.U32();
        if (length < 8 || length > MAX_STARTUP) throw WireError("08P01", "Invalid startup packet size", true);
        Buffer body(length - 4);
        if (!Receive(socket, body.data(), body.size())) return false;
        Reader in(body); auto protocol = in.U32();
        if (protocol == 80877103 || protocol == 80877104) {
            in.End();
            if (protocol == 80877103 && server.protected_mode && !socket.tls) {
                const char accepted = 'S'; Send(socket, &accepted, 1); UpgradeTLS();
            } else { const char denied = 'N'; Send(socket, &denied, 1); }
            continue;
        }
        if (protocol == 80877102) {
            auto target = in.U32(); auto secret = in.U32(); in.End(); server.Cancel(target, secret); return false;
        }
        if (server.protected_mode && !socket.tls) throw WireError("28000", "TLS is required", true);
        if (protocol != 196608) throw WireError("0A000", "Only PostgreSQL protocol 3.0 is supported", true);
        std::map<std::string, std::string> options;
        while (true) {
            auto key = in.String(); if (key.empty()) break;
            auto value = in.String();
            if (!options.emplace(key, value).second) throw WireError("08P01", "Duplicate startup option", true);
        }
        in.End();
        auto encoding = options.find("client_encoding");
        if (encoding != options.end() && Upper(encoding->second) != "UTF8" && Upper(encoding->second) != "UTF-8")
            throw WireError("0A000", "Only UTF8 client encoding is supported", true);
        Buffer auth;
        if (server.protected_mode) Authenticate(options);
        else {
            Put32(auth, 3); Message(socket, 'R', auth);
            const auto bytes = AuthenticationResponse(); Reader password_reader(bytes);
            const auto password = password_reader.String(); password_reader.End();
            std::size_t difference = password.size() ^ server.password.size();
            for (std::size_t i = 0; i < std::max(password.size(), server.password.size()); ++i)
                difference |= (i < password.size() ? static_cast<unsigned char>(password[i]) : 0) ^
                              (i < server.password.size() ? static_cast<unsigned char>(server.password[i]) : 0);
            if (difference) throw WireError("28P01", "Password authentication failed", true);
        }
        InitializeTypeCatalog();
        auth.clear(); Put32(auth, 0); Message(socket, 'R', auth);
        const std::array<std::pair<const char *, const char *>, 9> settings {{
            {"server_version", "16.0"}, {"server_encoding", "UTF8"}, {"client_encoding", "UTF8"},
            {"DateStyle", "ISO, MDY"}, {"TimeZone", "UTC"}, {"integer_datetimes", "on"},
            {"standard_conforming_strings", "on"}, {"is_superuser", "off"},
            {"session_authorization", "duckonwire"}
        }};
        for (const auto &setting : settings) {
            Buffer response; PutString(response, setting.first); PutString(response, server.protected_mode && std::string(setting.first) == "session_authorization" ? authenticated_user : setting.second); Message(socket, 'S', response);
        }
        Buffer identity; PutString(identity, "duckonwire_engine"); PutString(identity, duckdb::DuckDB::LibraryVersion()); Message(socket, 'S', identity);
        Buffer key; Put32(key, id); Put32(key, cancel_secret); Message(socket, 'K', key);
        if (!server.protected_mode) {
            auto result = connection.Query("SET TimeZone='UTC'");
            if (result->HasError()) NativeError(result->GetErrorObject());
        }
        Ready();
        socket.read_deadline = std::chrono::steady_clock::time_point::max();
        timeval idle_timeout {0, 0};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &idle_timeout, sizeof(idle_timeout));
        return true;
    }
    throw WireError("08P01", "Too many encryption negotiation requests", true);
}
void PgWireServer::Impl::Session::InitializeTypeCatalog() {
    // The wire catalog describes our codecs, not DuckDB's internal types. All objects are session-local temp views.
    // PostgreSQL arrays, ranges, domains, enums and composites have no wire codec and are deliberately absent.
    static const char *const metadata[] = {
        ("CREATE TEMP VIEW pg_namespace AS SELECT 11::UINTEGER oid,'pg_catalog'::VARCHAR nspname,"
        "0::UINTEGER nspowner,NULL::VARCHAR nspacl UNION ALL SELECT oid::UINTEGER,schema_name,"
        "0::UINTEGER,NULL::VARCHAR FROM duckdb_schemas() WHERE database_name=current_database() AND oid<>11"),
        ("CREATE TEMP VIEW pg_type AS SELECT oid::UINTEGER oid,typname::VARCHAR typname,11::UINTEGER typnamespace,"
        "0::UINTEGER typowner,typlen::SMALLINT typlen,typlen>0 AND typlen<=8 typbyval,typtype::VARCHAR typtype,"
        "category::VARCHAR typcategory,false typispreferred,true typisdefined,','::VARCHAR typdelim,"
        "0::UINTEGER typrelid,0::UINTEGER typelem,0::UINTEGER typarray,0::UINTEGER typreceive,"
        "false typnotnull,0::UINTEGER typbasetype FROM (VALUES "
        "(16,'bool',1,'b','B'),(17,'bytea',-1,'b','U'),(18,'char',1,'b','S'),(19,'name',64,'b','S'),"
        "(20,'int8',8,'b','N'),(21,'int2',2,'b','N'),(23,'int4',4,'b','N'),(25,'text',-1,'b','S'),"
        "(26,'oid',4,'b','N'),(114,'json',-1,'b','U'),(700,'float4',4,'b','N'),(701,'float8',8,'b','N'),"
        "(1042,'bpchar',-1,'b','S'),(1043,'varchar',-1,'b','S'),(1082,'date',4,'b','D'),"
        "(1083,'time',8,'b','D'),(1114,'timestamp',8,'b','D'),(1184,'timestamptz',8,'b','D'),"
        "(1186,'interval',16,'b','T'),(1266,'timetz',12,'b','D'),(1700,'numeric',-1,'b','N'),"
        "(2950,'uuid',16,'b','U'),(3802,'jsonb',-1,'b','U'),(705,'unknown',-1,'p','X')"
        ") types(oid,typname,typlen,typtype,category)"),
        ("CREATE TEMP VIEW pg_range AS SELECT NULL::UINTEGER rngtypid,NULL::UINTEGER rngsubtype,"
        "NULL::UINTEGER rngmultitypid,NULL::UINTEGER rngcollation,NULL::UINTEGER rngcanonical,"
        "NULL::UINTEGER rngsubdiff WHERE FALSE"),
        ("CREATE TEMP VIEW pg_enum AS SELECT NULL::UINTEGER oid,NULL::UINTEGER enumtypid,"
        "NULL::FLOAT enumsortorder,NULL::VARCHAR enumlabel WHERE FALSE")
    };
    for (const auto sql : metadata) {
        auto result = connection.Query(sql);
        if (result->HasError()) NativeError(result->GetErrorObject());
    }
}
void PgWireServer::Impl::Session::Run() {
    try {
        if (server.protected_mode) {
            const auto flags = fcntl(socket.fd, F_GETFL, 0);
            if (flags < 0 || fcntl(socket.fd, F_SETFL, flags | O_NONBLOCK)) throw Disconnected();
            socket.nonblocking = true;
        }
        if (!Startup()) { CloseSocket(); return; }
        while (!server.stopping.load()) {
            if (server.protected_mode) socket.read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
            unsigned char type; std::array<unsigned char, 4> header {};
            if (!Receive(socket, &type, 1) || !Receive(socket, header.data(), 4)) break;
            Buffer header_buffer(header.begin(), header.end()); Reader size(header_buffer); auto length = size.U32();
            if (length < 4 || length > MAX_FRAME) throw WireError("08P01", "Invalid PostgreSQL frame size", true);
            Buffer body(length - 4); if (!body.empty() && !Receive(socket, body.data(), body.size())) break;
            if (type == 'X') { Reader in(body); in.End(); break; }
            if (skip_until_sync && type != 'S') continue;
            try { Handle(static_cast<char>(type), body); }
            catch (const WireError &error) {
                RecordFailure(error.code, error.fatal);
                ErrorMessage(socket, error); if (error.fatal) break;
                if (type == 'Q') Ready(); else skip_until_sync = true;
            } catch (const std::exception &error) {
                const duckdb::ErrorData native(error);
                RecordFailure(ErrorState(native));
                ErrorMessage(socket, WireError(ErrorState(native), native.RawMessage()));
                if (type == 'Q') Ready(); else skip_until_sync = true;
            }
        }
    } catch (const WireError &error) {
        try { ErrorMessage(socket, error); } catch (...) {}
    } catch (...) {}
    portals.clear(); statements.clear();
    try { if (!connection.IsAutoCommit()) connection.Rollback(); } catch (...) {}
    CloseSocket();
}
void PgWireServer::Impl::Session::ResetConnection(bool preserve_statements) {
    TransactionFinished();
    portals.clear();
    if (!connection.IsAutoCommit()) connection.Rollback();
    if (preserve_statements) for (auto &entry : statements) entry.second->prepared.reset();
    else statements.clear();
    duckdb::Connection replacement(server.database);
    { std::lock_guard<std::mutex> guard(connection_lock); connection = std::move(replacement); }
    failed_transaction = false; implicit_extended_transaction = false;
    InitializeTypeCatalog();
    if (!server.protected_mode) {
        auto result = connection.Query("SET TimeZone='UTC'");
        if (result->HasError()) NativeError(result->GetErrorObject());
    }
}
void PgWireServer::Impl::Session::EnsurePrepared(Statement &statement) {
    if (!statement.prepared && !statement.empty && !statement.setting && !statement.deallocate && statement.control.kind == FdwKind::NONE) {
        if (server.protected_mode) {
            ParseAdmission admission(*this);
            auto parsed = Extract(statement.sql, &admission);
            if (parsed.size() != 1) throw WireError("42601", "A prepared statement must contain a single command");
            Authorize(*parsed[0]);
            statement.prepared = statement.type == duckdb::StatementType::EXPLAIN_STATEMENT
                ? connection.Prepare(statement.sql) : connection.Prepare(std::move(parsed[0]));
            admission.Check();
        } else statement.prepared = connection.Prepare(statement.sql);
        if (statement.prepared->HasError()) NativeError(statement.prepared->GetErrorObject());
        if (statement.prepared->GetParameterCount() != statement.parameter_oids.size())
            throw WireError("0A000", "Prepared statement parameter count changed after session reset");
    }
}

namespace {
bool SameParserOptions(const duckdb::ParserOptions &a, const duckdb::ParserOptions &b) {
    return a.identifier_case_mode == b.identifier_case_mode && a.integer_division == b.integer_division &&
           a.heap_based_parser == b.heap_based_parser &&
           a.regex_match_operator_semantics == b.regex_match_operator_semantics &&
           a.max_expression_depth == b.max_expression_depth &&
           a.parser_override_setting == b.parser_override_setting && a.extensions.get() == b.extensions.get() &&
           a.compiled_grammar.get() == b.compiled_grammar.get();
}
bool TemplateParser(const duckdb::ParserOptions &options, const duckdb::ClientConfig &client) {
    return options.parser_override_setting == duckdb::AllowParserOverride::DEFAULT_OVERRIDE &&
           options.compiled_grammar && !client.cached_grammar && client.active_grammar_extensions.empty();
}
// Logical retention bounds include all accepted SELECT
// AST children/containers/strings; these are not a hard process-RSS guarantee.
struct SelectTemplateBudget {
    std::size_t nodes=0, strings=0;
    bool AddNode(std::size_t depth) { return depth<=32 && ++nodes<=512; }
    bool Text(const std::string &text) { if(text.size()>8192 || strings>64*1024-text.size())return false; strings+=text.size();return true; }
    bool Name(const duckdb::Identifier &name) { return Text(name.GetIdentifierName()); }
    bool Qualified(const duckdb::QualifiedName &name) {
        if(name.Path().size()>4)return false;
        for(auto &part:name.Path())if(!Name(part))return false;
        return true;
    }
    bool Expression(const duckdb::ParsedExpression &expr,std::size_t depth) {
        if(!AddNode(depth) || !Name(expr.GetAlias()))return false;
        using C=duckdb::ExpressionClass;
        switch(expr.GetExpressionClass()) {
        case C::CONSTANT: {
            const auto &literal=expr.Cast<duckdb::ConstantExpression>().GetLiteral();
            if(literal.kind==duckdb::LiteralKind::POINTER || literal.kind==duckdb::LiteralKind::INVALID || !Text(literal.text))return false;
            break;
        }
        case C::COLUMN_REF:
            if(expr.Cast<duckdb::ColumnRefExpression>().ColumnNames().size()>4)return false;
            for(auto &name:expr.Cast<duckdb::ColumnRefExpression>().ColumnNames())if(!Name(name))return false;
            break;
        case C::PARAMETER: if(!Name(expr.Cast<duckdb::ParameterExpression>().Identifier()))return false; break;
        case C::TYPE: if(!Qualified(expr.Cast<duckdb::TypeExpression>().GetQualifiedName()))return false;break;
        case C::FUNCTION: {
            const auto &function=expr.Cast<duckdb::FunctionExpression>();
            if(!Qualified(function.GetQualifiedName()) || function.GetArguments().size()>64)return false;
            for(auto &arg:function.GetArguments())if(!Name(arg.GetName()))return false;
            break;
        }
        case C::WINDOW: {
            const auto &window=expr.Cast<duckdb::WindowExpression>();
            if(!Qualified(window.GetQualifiedName()) || window.GetArguments().size()>64 || window.Partitions().size()>64 ||
               window.OrderBy().size()>64 || window.ArgOrders().size()>64)return false;
            for(auto &arg:window.GetArguments())if(!Name(arg.GetName()))return false;
            // Native Children includes partitions, ordering, filter, frame bounds,
            // argument ordering and all arguments in this exact engine pin.
            break;
        }
        case C::STAR: {
            const auto &star=expr.Cast<duckdb::StarExpression>();
            if(!star.RelationName().empty() || !star.ExcludeList().empty() || !star.ReplaceList().empty() ||
               !star.RenameList().empty() || star.Expression() || star.IsColumns())return false;
            break;
        }
        case C::CAST:
            // Native Children omits the unbound cast target; explicitly charge it.
            if(!Expression(expr.Cast<duckdb::CastExpression>().TargetType(),depth+1))return false;
            break;
        case C::CASE: case C::COMPARISON: case C::CONJUNCTION: case C::OPERATOR: case C::BETWEEN: break;
        // Conservatively bypass scalar subqueries, lambdas/collations,
        // complex stars and every extension/bound expression instead of guessing
        // about auxiliary ownership. Ordinary derived tables/CTEs are covered.
        default:return false;
        }
        for(auto &child:expr.Children())if(!Expression(child,depth+1))return false;
        return true;
    }
    bool Reference(const duckdb::TableRef &ref,std::size_t depth) {
        if(!AddNode(depth) || ref.sample || ref.external_dependency || !Name(ref.alias) || ref.column_name_alias.size()>64)return false;
        for(auto &name:ref.column_name_alias)if(!Name(name))return false;
        using R=duckdb::TableReferenceType;
        switch(ref.type) {
        case R::EMPTY_FROM:return true;
        case R::BASE_TABLE: {
            const auto &base=ref.Cast<duckdb::BaseTableRef>();
            return !base.at_clause && Qualified(base.GetQualifiedName());
        }
        case R::SUBQUERY: {
            auto &sub=ref.Cast<duckdb::SubqueryRef>().subquery;
            return sub && sub->named_param_map.empty() && Text(sub->query) && Query(*sub->node,depth+1);
        }
        case R::JOIN: {
            auto &join=ref.Cast<duckdb::JoinRef>();
            if(!join.left || !join.right || !join.duplicate_eliminated_columns.empty() || join.ranking_expression || join.using_columns.size()>64)return false;
            for(auto &name:join.using_columns)if(!Name(name))return false;
            return Reference(*join.left,depth+1) && Reference(*join.right,depth+1) && (!join.condition || Expression(*join.condition,depth+1));
        }
        default:return false;
        }
    }
    bool Query(const duckdb::QueryNode &node,std::size_t depth) {
        if(!AddNode(depth) || node.type!=duckdb::QueryNodeType::SELECT_NODE || node.cte_map.map.size()>32 || node.modifiers.size()>8)return false;
        for(auto &cte:node.cte_map.map) {
            auto &info=*cte.second;
            if(!Name(cte.first) || info.is_trigger_generated || !info.query_node || !info.key_targets.empty() ||
               !info.payload_aggregates.empty() || info.aliases.size()>64)return false;
            for(auto &name:info.aliases)if(!Name(name))return false;
            if(!Query(*info.query_node,depth+1))return false;
        }
        auto &select=node.Cast<duckdb::SelectNode>();
        // Native CUBE/ROLLUP can expand grouping-set containers independently
        // of expressions. Only a single ordinary grouping set is admitted.
        if(select.sample || select.select_list.size()>64 || select.groups.group_expressions.size()>64 || select.groups.grouping_sets.size()>1)return false;
        for(auto &set:select.groups.grouping_sets)if(set.size()>64)return false;
        for(auto &expr:select.select_list)if(!Expression(*expr,depth+1))return false;
        for(auto &expr:select.groups.group_expressions)if(!Expression(*expr,depth+1))return false;
        if((select.where_clause && !Expression(*select.where_clause,depth+1)) ||
           (select.having && !Expression(*select.having,depth+1)) ||
           (select.qualify && !Expression(*select.qualify,depth+1)) ||
           !select.from_table || !Reference(*select.from_table,depth+1))return false;
        for(auto &mod:node.modifiers) {
            if(!AddNode(depth+1))return false;
            using M=duckdb::ResultModifierType;
            if(mod->type==M::LIMIT_MODIFIER) {
                auto &limit=mod->Cast<duckdb::LimitModifier>();
                if((limit.limit && !Expression(*limit.limit,depth+1)) || (limit.offset && !Expression(*limit.offset,depth+1)))return false;
            }else if(mod->type==M::ORDER_MODIFIER) {
                auto &order=mod->Cast<duckdb::OrderModifier>();if(order.orders.size()>64)return false;
                for(auto &entry:order.orders)if(!Expression(*entry.expression,depth+1))return false;
            }else if(mod->type==M::DISTINCT_MODIFIER) {
                auto &distinct=mod->Cast<duckdb::DistinctModifier>();if(distinct.distinct_on_targets.size()>64)return false;
                for(auto &expr:distinct.distinct_on_targets)if(!Expression(*expr,depth+1))return false;
            }else return false;
        }
        return true;
    }
};
bool BoundedSelectTemplate(const duckdb::SQLStatement &statement,const std::string &sql,SelectTemplateBudget &budget) {
    const auto word=FirstWord(sql);
    if((word!="SELECT" && word!="WITH") || sql.size()>8192 || statement.type!=duckdb::StatementType::SELECT_STATEMENT || statement.named_param_map.size()>64)return false;
    if(!budget.Text(sql) || !budget.Text(statement.query))return false;
    for(auto &param:statement.named_param_map)if(!budget.Name(param.first))return false;
    return budget.Query(*statement.Cast<duckdb::SelectStatement>().node,0);
}

bool PositionalValuesTemplate(const duckdb::SQLStatement &statement, const std::string &sql) {
    // Bound the retained shape, not just the SQL length. Every retained data cell
    // is one parameter node; literals, expressions and catalog-derived trees stay
    // on the ordinary native path. The single slot is replaced rather than grown.
    if (FirstWord(sql) != "INSERT" || sql.size() > 128 * 1024 || sql.find('?') != std::string::npos ||
        statement.type != duckdb::StatementType::INSERT_STATEMENT ||
        !statement.has_anonymous_parameters || statement.named_param_map.empty() ||
        statement.named_param_map.size() > 4096) return false;
    const auto &insert = statement.Cast<duckdb::InsertStatement>();
    const auto &node = *insert.node;
    if (!node.cte_map.map.empty() || !node.modifiers.empty() || node.on_conflict_info ||
        !node.returning_list.empty() || node.default_values || node.column_order != duckdb::InsertColumnOrder::INSERT_BY_POSITION || node.columns.size() > 64 ||
        node.qualified_name.Path().size() > 4) return false;
    std::size_t name_bytes = 0;
    for (const auto &part : node.qualified_name.Path()) name_bytes += part.GetIdentifierName().size();
    if (name_bytes > 512) return false;
    for (const auto &column : node.columns) if (column.GetIdentifierName().size() > 128) return false;
    // Inspect the native VALUES representation directly; the corresponding
    // convenience helpers are not part of DuckDB's exported extension ABI.
    if (!node.select_statement || node.select_statement->node->type != duckdb::QueryNodeType::SELECT_NODE) return false;
    const auto &select = node.select_statement->node->Cast<duckdb::SelectNode>();
    if (!select.modifiers.empty() || !select.cte_map.map.empty() || select.where_clause || select.having ||
        select.qualify || select.sample || !select.groups.grouping_sets.empty() || !select.groups.group_expressions.empty() ||
        select.aggregate_handling != duckdb::AggregateHandling::STANDARD_HANDLING || select.select_list.size() != 1 ||
        select.select_list[0]->GetExpressionClass() != duckdb::ExpressionClass::STAR || !select.from_table ||
        select.from_table->type != duckdb::TableReferenceType::EXPRESSION_LIST) return false;
    const auto &star = select.select_list[0]->Cast<duckdb::StarExpression>();
    if (!star.RelationName().empty() || !star.ExcludeList().empty() || !star.ReplaceList().empty() ||
        !star.RenameList().empty() || star.Expression() || star.IsColumns()) return false;
    const auto &values = select.from_table->Cast<duckdb::ExpressionListRef>();
    if (!values.expected_types.empty() || !values.expected_names.empty() ||
        values.values.empty() || values.values.size() > 4096) return false;
    std::size_t cells = 0;
    for (const auto &row : values.values) {
        if (row.empty() || row.size() > 64) return false;
        for (const auto &expression : row) {
            if (++cells > 4096 || expression->GetExpressionClass() != duckdb::ExpressionClass::PARAMETER) return false;
            const auto &name = expression->Cast<duckdb::ParameterExpression>().Identifier().GetIdentifierName();
            if (name != std::to_string(cells)) return false;
        }
    }
    return cells == statement.named_param_map.size();
}
} // namespace

duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>>
PgWireServer::Impl::Session::Extract(const std::string &sql, ParseAdmission *admission) {
    if (!server.protected_mode) return connection.ExtractStatements(sql);
    std::unique_ptr<ParseAdmission> own_admission;
    if (!admission) { own_admission = std::make_unique<ParseAdmission>(*this); admission = own_admission.get(); }
    admission->Check();
    const auto parser_options = connection.context->GetParserOptions();
    const bool template_parser = TemplateParser(parser_options, duckdb::ClientConfig::GetConfig(*connection.context));
    duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> result;
    if (template_parser) {
        for (auto &entry:server.select_templates) {
            if (entry && sql==entry->sql && SameParserOptions(parser_options,entry->options)) {
                result.push_back(entry->statement->Copy());
                entry->touched=++server.select_clock;
                admission->Check(); return result;
            }
        }
    }
    if (server.insert_template && sql == server.insert_template->sql && template_parser &&
        SameParserOptions(parser_options, server.insert_template->options)) {
        // No shared prepared/bound state: each caller owns its copy and is still
        // authorized and prepared against its own current transaction/catalog.
        result.push_back(server.insert_template->statement->Copy());
    } else {
        result = connection.ExtractStatements(sql);
        admission->Check();
        // DEFAULT_OVERRIDE skips parser overrides; a successful native INSERT
        // cannot be a parse_function fallback (those are ExtensionStatement).
        if (result.size() == 1 && template_parser && PositionalValuesTemplate(*result[0], sql)) {
            // Publish the tuple only after every allocation succeeds. A failed
            // replacement must never pair the old SQL key with a new AST.
            auto next = std::make_unique<Impl::InsertTemplate>();
            next->sql = sql; next->options = parser_options; next->statement = result[0]->Copy();
            server.insert_template = std::move(next);
        }
        SelectTemplateBudget budget;
        if(result.size()==1 && template_parser && BoundedSelectTemplate(*result[0],sql,budget)) {
            auto next=std::make_unique<Impl::SelectTemplate>();
            next->sql=sql; next->options=parser_options; next->statement=result[0]->Copy();
            next->nodes=budget.nodes; next->strings=budget.strings;
            server.PublishSelect(std::move(next));
        }
    }
    admission->Check();
    return result;
}
void PgWireServer::Impl::Session::RecordFailure(const std::string &state, bool fatal) {
    if (connection.IsAutoCommit()) return;
    if (!failed_transaction) recoverable_failure = true;
    if (state != "25P02") {
        const bool known = state.compare(0, 2, "22") == 0 || state.compare(0, 2, "42") == 0 ||
                           state == "57014" || state == "53300" || state == "54000" ||
                           state == "34000" || state == "3B001" || state == "0A000" || state == "55000";
        recoverable_failure = recoverable_failure && known && !fatal;
    }
    failed_transaction = true;
}
void PgWireServer::Impl::Session::ReadOnlyTransaction() {
    if (!server.protected_mode || !read_only || connection.IsAutoCommit()) return;
    connection.context->RunFunctionInTransaction([&] { connection.context->transaction.SetReadOnly(); }, false);
}
void PgWireServer::Impl::Session::SavepointControl(const FdwCommand &command) {
    if (!server.protected_mode || !read_only)
        throw WireError("0A000", "SAVEPOINT is supported only for protected read-only database users");
    if (connection.IsAutoCommit() || implicit_extended_transaction || implicit_simple_transaction)
        throw WireError("25P01", "SAVEPOINT requires an explicit read-only transaction");
    ReadOnlyTransaction();
    if (command.kind == FdwKind::SAVEPOINT) {
        if (failed_transaction) throw WireError("25P02", "Current transaction is aborted; issue ROLLBACK TO SAVEPOINT");
        if (savepoints.size() >= MAX_OBJECTS) throw WireError("54000", "Too many savepoints");
        const auto &transaction = connection.context->transaction.ActiveTransaction();
        savepoints.push_back({command.name, transaction.global_transaction_id, resource_generation});
    } else {
        auto target = savepoints.size();
        while (target && savepoints[target - 1].name != command.name) --target;
        if (!target) throw WireError("3B001", "Unknown savepoint");
        const auto frame = savepoints[target - 1];
        if (command.kind == FdwKind::RELEASE) {
            if (failed_transaction) throw WireError("25P02", "Current transaction is aborted; issue ROLLBACK TO SAVEPOINT");
            savepoints.resize(target - 1);
        } else {
            if (failed_transaction && !recoverable_failure)
                throw WireError("25P02", "This transaction error requires a full ROLLBACK");
            auto &context = *connection.context;
            context.RunFunctionInTransaction([&] {
                auto &transaction = context.transaction.ActiveTransaction();
                if (transaction.global_transaction_id != frame.transaction_id) throw WireError("25P02", "Savepoint transaction no longer exists; issue ROLLBACK");
                if (!transaction.IsReadOnly() || transaction.ModifiedDatabase()) throw WireError("25P02", "Savepoint requires an unmodified read-only transaction; issue ROLLBACK");
                if (transaction.GetActiveQuery() != duckdb::MAXIMUM_QUERY_ID)
                    throw WireError("25P02", "Savepoint requires a finalized query; issue ROLLBACK");
                if (duckdb::ValidChecker::IsInvalidated(*context.db)) throw WireError("25P02", "The native database is invalid; issue ROLLBACK");
                for (auto &database : transaction.OpenedTransactions()) {
                    auto native = transaction.TryGetTransaction(database.get());
                    if (!native || !native->IsReadOnly()) throw WireError("25P02", "Savepoint contains a modified native transaction; issue ROLLBACK");
                    if (native->active_query != duckdb::MAXIMUM_QUERY_ID) throw WireError("25P02", "Savepoint native query is still active; issue ROLLBACK");
                    if (duckdb::ValidChecker::IsInvalidated(database.get())) throw WireError("25P02", "Savepoint attached database is invalid; issue ROLLBACK");
                }
                if (duckdb::ValidChecker::IsInvalidated(transaction)) {
                    // The pinned native API has no validity reset. Reconstruct
                    // only this finalized, unmodified read transaction's checker;
                    // the MetaTransaction and all MVCC snapshots remain intact.
                    auto &validity = transaction.transaction_validity;
                    validity.~ValidChecker();
                    new (static_cast<void *>(&validity)) duckdb::ValidChecker(*context.db);
                }
            }, false);
            for (auto cursor = sql_cursors.begin(); cursor != sql_cursors.end();) {
                if (cursor->second.generation > frame.generation) cursor = sql_cursors.erase(cursor); else ++cursor;
            }
            // Portals are retired by the caller after its control response, so
            // the currently executing ROLLBACK portal never becomes dangling.
            savepoints.resize(target);
            failed_transaction = false; recoverable_failure = true;
        }
    }
}
void PgWireServer::Impl::Session::Handle(char type, const Buffer &body) {
    Reader in(body);
    switch (type) {
    case 'Q': { auto sql = in.String(); in.End(); Simple(sql); break; }
    case 'P': Parse(in); break;
    case 'B': Bind(in); break;
    case 'D': Describe(in); break;
    case 'E': Execute(in); break;
    case 'C': {
        auto object = in.Byte(); auto name = in.String(); in.End();
        if (object == 'S') {
            auto statement = statements.find(name);
            if (statement != statements.end()) {
                for (auto portal = portals.begin(); portal != portals.end();) {
                    if (portal->second.statement == statement->second) portal = portals.erase(portal); else ++portal;
                }
                statements.erase(statement);
            }
        }
        else if (object == 'P') portals.erase(name);
        else throw WireError("08P01", "Invalid Close object type", true);
        Message(socket, '3'); break;
    }
    case 'S':
        in.End(); skip_until_sync = false;
        if (implicit_extended_transaction) {
            try {
                if (failed_transaction) connection.Rollback(); else connection.Commit();
            } catch (const std::exception &error) {
                try { if (!connection.IsAutoCommit()) connection.Rollback(); } catch (...) {}
                const duckdb::ErrorData native(error);
                ErrorMessage(socket, WireError(ErrorState(native), native.RawMessage()));
            }
            implicit_extended_transaction = false; failed_transaction = false;
        }
        if (connection.IsAutoCommit()) portals.clear();
        Ready(); break;
    case 'H': in.End(); break; // Responses are sent immediately; no buffered output remains.
    default: throw WireError("0A000", "Unsupported PostgreSQL frontend message");
    }
}

void PgWireServer::Impl::Session::FdwStore(Portal &&portal) {
    const auto name=portal.statement->fdw_cursor_name;
    if (name.empty() || sql_cursors.count(name)) throw WireError("42P03", "Cursor already exists");
    if (sql_cursors.size()>=MAX_OBJECTS) throw WireError("54000", "Too many SQL cursors");
    if (connection.IsAutoCommit() || implicit_extended_transaction || implicit_simple_transaction)
        throw WireError("25P01", "DECLARE requires an explicit transaction");
    if (portal.statement->type!=duckdb::StatementType::SELECT_STATEMENT) throw WireError("42501", "SQL cursors support SELECT only");
    ExecutionGuard execution(*this);
    ReadOnlyTransaction();
    auto signature=BoundSignature(*portal.statement,portal.parameters); AuthorizeBound(signature.properties);
    if (!signature.properties.IsReadOnly()) throw WireError("42501", "SQL cursors must be read-only");
    auto execute=duckdb::make_uniq<duckdb::ExecuteStatement>();
    execute->name=duckdb::Identifier(portal.statement->prepared->GetName());execute->query=portal.statement->sql;
    for(std::size_t i=0;i<portal.parameters.size();++i)
        execute->bound_values[duckdb::Identifier(std::to_string(i+1))]=duckdb::BoundParameterData(portal.parameters[i]);
    execution.StartTimer(); portal.result=connection.Query(std::move(execute),duckdb::QueryResultMemoryType::BUFFER_MANAGED);
    if(portal.result->HasError())NativeError(portal.result->GetErrorObject());
    portal.generation = ++resource_generation;
    sql_cursors.emplace(name,std::move(portal));
    Buffer done; PutString(done,"DECLARE CURSOR"); Message(socket,'C',done);
}
void PgWireServer::Impl::Session::FdwExecute(Portal &request, std::uint32_t maximum, bool simple) {
    const auto command = request.statement->control;
    GuardTransaction(request.statement->sql);
    if (command.kind == FdwKind::SAVEPOINT || command.kind == FdwKind::ROLLBACK_TO || command.kind == FdwKind::RELEASE) {
        SavepointControl(command);
        Buffer done; PutString(done, command.kind == FdwKind::SAVEPOINT ? "SAVEPOINT" : command.kind == FdwKind::RELEASE ? "RELEASE" : "ROLLBACK");
        Message(socket, 'C', done); request.complete = true; return;
    }
    if (command.kind == FdwKind::SETTING) {
        Buffer done; PutString(done, "SET"); Message(socket, 'C', done); request.complete = true; return;
    }
    if (command.kind == FdwKind::CLOSE || command.kind == FdwKind::CLOSE_ALL) {
        if (command.kind == FdwKind::CLOSE_ALL) sql_cursors.clear();
        else if (!sql_cursors.erase(command.name)) throw WireError("34000", "Unknown SQL cursor");
        Buffer done; PutString(done, "CLOSE CURSOR"); Message(socket, 'C', done); request.complete = true; return;
    }
    auto found = sql_cursors.find(command.name);
    if (found == sql_cursors.end()) throw WireError("34000", "Unknown SQL cursor");
    auto &cursor = found->second;
    if (cursor.failed) throw WireError("55000", "This SQL cursor cannot execute after an error");
    if (simple) RowDescription(cursor.result->GetNames(), cursor.result->GetTypes(), {}, nullptr);
    if (request.complete) { Buffer done; PutString(done, "FETCH " + std::to_string(request.sent)); Message(socket, 'C', done); return; }
    const auto cancel = cancel_generation.load();
    try {
        ExecutionGuard execution(*this); execution.StartTimer();
        std::uint32_t emitted = 0;
        while (request.sent < command.amount && !cursor.complete) {
            if (cancel_generation.load() != cancel) throw WireError("57014", "SQL cursor fetch was interrupted");
            if (!cursor.chunk || cursor.chunk_row >= cursor.chunk->size()) {
                cursor.chunk = cursor.result->Fetch(); cursor.chunk_row = 0;
                if (cursor.result->HasError()) NativeError(cursor.result->GetErrorObject());
                if (!cursor.chunk || !cursor.chunk->size()) { cursor.complete = true; break; }
            }
            if (maximum && emitted == maximum) { Message(socket, 's'); return; }
            Buffer row; Put16(row, static_cast<std::uint16_t>(cursor.result->ColumnCount()));
            for (std::size_t i = 0; i < cursor.result->ColumnCount(); ++i) {
                auto value = cursor.chunk->GetValue(i, cursor.chunk_row);
                if (value.IsNull()) { Put32(row, UINT32_MAX); continue; }
                auto field = Field(value, Format(request.result_formats, i, cursor.result->ColumnCount()));
                if (field.size() > MAX_FRAME - 4 || row.size() + field.size() + 4 > MAX_FRAME - 4)
                    throw WireError("54000", "Cursor row exceeds frame limit");
                Put32(row, static_cast<std::uint32_t>(field.size())); row.insert(row.end(), field.begin(), field.end());
            }
            Message(socket, 'D', row); ++request.sent; ++emitted; ++cursor.chunk_row; ++cursor.sent;
        }
        request.complete = true;
        Buffer done; PutString(done, "FETCH " + std::to_string(request.sent)); Message(socket, 'C', done);
    } catch (...) { cursor.failed = true; throw; }
}
bool PgWireServer::Impl::Session::FdwSimple(const std::string &sql) {
    const auto control = FdwControl(sql);
    FdwDeclaration declaration;
    if (control.kind == FdwKind::NONE && !FdwDeclare(sql, declaration)) return false;
    GuardTransaction(sql);
    if (control.kind != FdwKind::NONE) {
        Portal request; request.statement = std::make_shared<Statement>();
        request.statement->sql = sql; request.statement->control = control;
        FdwExecute(request, 0, true);
        if (control.kind == FdwKind::ROLLBACK_TO) {
            const auto generation = savepoints.back().generation;
            for (auto entry = portals.begin(); entry != portals.end();) {
                if (entry->second.generation > generation) entry = portals.erase(entry); else ++entry;
            }
        } else if (control.kind == FdwKind::CLOSE_ALL) portals.clear();
        Ready(); return true;
    }
    ReadOnlyTransaction();
    std::unique_ptr<ParseAdmission> admission;
    if (server.protected_mode) admission = std::make_unique<ParseAdmission>(*this);
    auto parsed = Extract(declaration.query, admission.get());
    if (parsed.size() != 1 || parsed[0]->type != duckdb::StatementType::SELECT_STATEMENT)
        throw WireError("42501", "SQL cursors support one SELECT only");
    Authorize(*parsed[0]);
    Portal portal; portal.statement = std::make_shared<Statement>(); portal.statement->sql = declaration.query;
    portal.statement->type = parsed[0]->type; portal.statement->fdw_cursor_name = declaration.name;
    portal.statement->prepared = connection.Prepare(std::move(parsed[0]));
    if (admission) admission->Check();
    // Execution is separately admitted and timed; release the parse slot before
    // running the cursor's SELECT.
    admission.reset();
    if (portal.statement->prepared->HasError()) NativeError(portal.statement->prepared->GetErrorObject());
    if (portal.statement->prepared->GetParameterCount()) throw WireError("42P02", "DECLARE parameter requires extended Bind");
    FdwStore(std::move(portal)); Ready(); return true;
}

void PgWireServer::Impl::Session::Simple(const std::string &input_sql) {
    const auto sql = NormalizeTransactions(input_sql);
    const auto controls = ControlCommands(sql);
    static const std::vector<std::string> pooled_reset {
        "SET SESSION AUTHORIZATION DEFAULT", "RESET ALL", "CLOSE ALL", "UNLISTEN *",
        "SELECT PG_ADVISORY_UNLOCK_ALL ( )", "DISCARD SEQUENCES", "DISCARD TEMP"
    };
    if (controls == std::vector<std::string> {"DISCARD ALL"} || controls == pooled_reset) {
        const bool preserve = controls == pooled_reset;
        ResetConnection(preserve);
        if (preserve) {
            for (const auto &tag : {"SET", "RESET", "CLOSE CURSOR", "UNLISTEN", "SELECT 1", "DISCARD SEQUENCES", "DISCARD TEMP"}) {
                if (std::string(tag) == "SELECT 1") {
                    Buffer description; Put16(description, 1); PutString(description, "pg_advisory_unlock_all");
                    Put32(description, 0); Put16(description, 0); Put32(description, 2278); Put16(description, static_cast<std::uint16_t>(-1));
                    Put32(description, static_cast<std::uint32_t>(-1)); Put16(description, 0); Message(socket, 'T', description);
                    Buffer row; Put16(row, 1); Put32(row, 0); Message(socket, 'D', row);
                }
                Buffer body; PutString(body, tag); Message(socket, 'C', body);
            }
        } else { Buffer body; PutString(body, "DISCARD ALL"); Message(socket, 'C', body); }
        Ready(); return;
    }
    const auto commands = SplitCommands(sql);
    bool custom = false;
    for (const auto &command : commands) {
        FdwDeclaration declaration;
        if (FdwControl(command).kind != FdwKind::NONE || FdwDeclare(command, declaration)) custom = true;
    }
    if (custom && commands.size() > 1) {
        bool implicit = connection.IsAutoCommit();
        if (implicit) { connection.BeginTransaction(); ReadOnlyTransaction(); implicit_simple_transaction = true; }
        ++suppress_ready;
        try {
            for (const auto &command : commands) {
                const auto word = FirstWord(command);
                if (implicit && (word == "BEGIN" || word == "START")) {
                    implicit = false; implicit_simple_transaction = false;
                    Buffer done; PutString(done, "BEGIN"); Message(socket, 'C', done); continue;
                }
                Simple(command);
                if (TransactionEnd(command) && FdwControl(command).kind != FdwKind::ROLLBACK_TO) implicit = false;
            }
            if (implicit) connection.Commit();
        } catch (...) {
            --suppress_ready; implicit_simple_transaction = false;
            if (implicit) { try { connection.Rollback(); } catch (...) {} TransactionFinished(); }
            throw;
        }
        --suppress_ready; implicit_simple_transaction = false; Ready(); return;
    }
    if (custom && !commands.empty() && FdwSimple(commands[0])) return;
    ReadOnlyTransaction();
    portals.erase(""); statements.erase("");
    if (DeallocateAll(sql)) {
        GuardTransaction(sql); portals.clear(); statements.clear();
        Buffer body; PutString(body, "DEALLOCATE"); Message(socket, 'C', body); Ready(); return;
    }
    auto extracted = Extract(sql);
    if (extracted.empty()) { Message(socket, 'I'); Ready(); return; }
    for (auto &statement : extracted) {
        if (statement->type != duckdb::StatementType::SET_STATEMENT || !SessionSetting(statement->ToString())) Authorize(*statement);
    }
    const bool batch = extracted.size() > 1;
    bool implicit = batch && connection.IsAutoCommit();
    if (implicit) { connection.BeginTransaction(); ReadOnlyTransaction(); }
    try {
        for (auto &statement : extracted) {
            const auto text = statement->ToString(); GuardTransaction(text);
            const auto word = FirstWord(text);
            const bool transaction = statement->type == duckdb::StatementType::TRANSACTION_STATEMENT;
            if (transaction && (word == "BEGIN" || word == "START") && implicit) {
                // PostgreSQL BEGIN converts the surrounding simple-query implicit batch into an explicit transaction.
                implicit = false; Buffer body; PutString(body, "BEGIN"); Message(socket, 'C', body); continue;
            }
            if (batch && connection.IsAutoCommit() && !transaction) { connection.BeginTransaction(); ReadOnlyTransaction(); implicit = true; }
            if (failed_transaction && TransactionEnd(text)) {
                connection.Rollback(); failed_transaction = false;
                TransactionFinished();
                Buffer body; PutString(body, "ROLLBACK"); Message(socket, 'C', body); continue;
            }
            if (SessionSetting(text)) { Buffer body; PutString(body, "SET"); Message(socket, 'C', body); continue; }
            Portal portal; portal.statement = std::make_shared<Statement>(); portal.statement->sql = text;
            portal.statement->type = statement->type;
            const bool read_transaction = server.protected_mode && read_only && connection.IsAutoCommit() &&
                                          statement->type != duckdb::StatementType::TRANSACTION_STATEMENT;
            try {
                ExecutionGuard execution(*this);
                if (read_transaction) connection.BeginTransaction();
                ReadOnlyTransaction();
                if (server.protected_mode && read_only && statement->type != duckdb::StatementType::TRANSACTION_STATEMENT) {
                    auto signature = connection.context->BindStatement(statement->Copy()); AuthorizeBound(signature.properties);
                }
                execution.StartTimer(); portal.result = connection.Query(std::move(statement));
            } catch (...) {
                if (read_transaction) { try { if (!connection.IsAutoCommit()) connection.Rollback(); } catch (...) {} }
                throw;
            }
            if (portal.result->HasError()) {
                if (read_transaction) { try { if (!connection.IsAutoCommit()) connection.Rollback(); } catch (...) {} }
                NativeError(portal.result->GetErrorObject());
            }
            if (read_transaction) connection.Commit();
            if (portal.result->GetStatementProperties().return_type == duckdb::StatementReturnType::QUERY_RESULT)
                RowDescription(portal.result->GetNames(), portal.result->GetTypes(), {}, &portal);
            Rows(portal, 0);
            if (transaction && TransactionEnd(text)) TransactionFinished();
            else if (transaction) ReadOnlyTransaction();
            if (transaction) implicit = false;
            if (connection.IsAutoCommit()) failed_transaction = false;
        }
        if (implicit) connection.Commit();
    } catch (...) {
        if (implicit) { try { connection.Rollback(); } catch (...) {} failed_transaction = false; }
        throw;
    }
    Ready();
}
void PgWireServer::Impl::Session::Parse(Reader &in) {
    auto name = in.String(); auto sql = NormalizeTransactions(in.String()); auto count = in.U16();
    const auto control = FdwControl(sql);
    FdwDeclaration declaration; if(FdwDeclare(sql,declaration)) sql=declaration.query;
    std::vector<std::uint32_t> oids; for (unsigned i = 0; i < count; ++i) oids.push_back(in.U32()); in.End();
    for (auto oid : oids) if (oid) DuckType(oid);
    GuardTransaction(sql);
    ReadOnlyTransaction();
    if (!name.empty() && statements.count(name)) throw WireError("42P05", "Prepared statement already exists");
    if (statements.size() >= MAX_OBJECTS && !statements.count(name)) throw WireError("54000", "Too many prepared statements");
    auto deallocate = DeallocateAll(sql);
    std::unique_ptr<ParseAdmission> admission;
    if (server.protected_mode && !deallocate && control.kind == FdwKind::NONE)
        admission = std::make_unique<ParseAdmission>(*this);
    auto extracted = deallocate || control.kind != FdwKind::NONE
        ? duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>>() : Extract(sql, admission.get());
    if (!declaration.name.empty() && (extracted.size()!=1 || extracted[0]->type!=duckdb::StatementType::SELECT_STATEMENT)) throw WireError("42501","SQL cursors support one SELECT only");
    if (extracted.size() > 1) throw WireError("42601", "A prepared statement must contain a single command");
    auto statement = std::make_shared<Statement>(); statement->sql = sql; statement->fdw_cursor_name=declaration.name;
    statement->control = control;
    statement->empty = extracted.empty() && !deallocate && control.kind == FdwKind::NONE;
    if (!extracted.empty()) statement->type = extracted[0]->type;
    statement->deallocate = deallocate;
    statement->setting = !statement->empty && SessionSetting(sql);
    if (!extracted.empty() && !statement->setting) Authorize(*extracted[0]);
    if (!statement->empty && !statement->setting && !statement->deallocate && control.kind == FdwKind::NONE) {
        // The string path initializes the native EXPLAIN ANALYZE profiler.
        statement->prepared = statement->type == duckdb::StatementType::EXPLAIN_STATEMENT
            ? connection.Prepare(sql) : connection.Prepare(std::move(extracted[0]));
        if (admission) admission->Check();
        if (statement->prepared->HasError()) NativeError(statement->prepared->GetErrorObject());
        auto parameters = statement->prepared->GetParameterCount();
        if (oids.size() > parameters) throw WireError("08P01", "Too many declared parameter types");
        oids.resize(parameters, 0);
        for (std::size_t i = 0; i < parameters; ++i) {
            if (!oids[i]) {
                LogicalType type;
                if (statement->prepared->TryGetParameterType(duckdb::Identifier(std::to_string(i + 1)), type))
                    oids[i] = PGType(type).oid;
            }
        }
    } else if (!oids.empty()) throw WireError("08P01", "This command has no parameters");
    statement->parameter_oids = std::move(oids);
    statements[name] = std::move(statement); Message(socket, '1');
}
void PgWireServer::Impl::Session::Bind(Reader &in) {
    auto name = in.String(); auto source = in.String();
    auto found = statements.find(source);
    if (found == statements.end()) throw WireError("26000", "Unknown prepared statement");
    GuardTransaction(found->second->sql);
    ReadOnlyTransaction();
    EnsurePrepared(*found->second);
    if (!name.empty() && portals.count(name)) throw WireError("42P03", "Portal already exists");
    if (portals.size() >= MAX_OBJECTS && !portals.count(name)) throw WireError("54000", "Too many portals");
    auto formats_count = in.U16(); std::vector<std::uint16_t> formats;
    for (unsigned i = 0; i < formats_count; ++i) formats.push_back(in.U16());
    auto count = in.U16();
    if (count != found->second->parameter_oids.size()) throw WireError("08P01", "Bound parameter count does not match statement");
    Portal portal; portal.statement = found->second; portal.generation = ++resource_generation;
    for (std::size_t i = 0; i < count; ++i) {
        auto format = Format(formats, i, count); auto length = static_cast<std::int32_t>(in.U32());
        if (format > 1) throw WireError("08P01", "Unknown parameter format");
        if (length == -1) portal.parameters.emplace_back(DuckType(portal.statement->parameter_oids[i]));
        else {
            if (length < 0) throw WireError("08P01", "Invalid parameter length", true);
            portal.parameters.push_back(Parameter(portal.statement->parameter_oids[i], format, in.Bytes(static_cast<std::size_t>(length))));
        }
    }
    if (count == 0 && formats.size() > 1) throw WireError("08P01", "Invalid parameter format count");
    auto results = in.U16();
    for (unsigned i = 0; i < results; ++i) {
        auto format = in.U16(); if (format > 1) throw WireError("08P01", "Unknown result format");
        portal.result_formats.push_back(format);
    }
    in.End();
    if (portal.statement->prepared) {
        portal.signature = duckdb::make_uniq<duckdb::StatementSignature>(BoundSignature(*portal.statement, portal.parameters));
        AuthorizeBound(portal.signature->properties);
        if (portal.statement->described) {
            const bool returns_rows = portal.signature->properties.return_type == duckdb::StatementReturnType::QUERY_RESULT;
            if (returns_rows != portal.statement->returns_rows ||
                (returns_rows && portal.signature->types.size() != portal.statement->described_oids.size()))
                throw WireError("0A000", "Bound result schema differs from prepared statement Describe; describe the bound portal");
            if (returns_rows) for (std::size_t i = 0; i < portal.signature->types.size(); ++i)
                if (PGType(portal.signature->types[i]).oid != portal.statement->described_oids[i])
                    throw WireError("0A000", "Bound result type differs from prepared statement Describe; describe the bound portal");
        }
    }
    portals[name] = std::move(portal); Message(socket, '2');
}
duckdb::StatementSignature PgWireServer::Impl::Session::BoundSignature(const Statement &statement,
                                                                       const duckdb::vector<Value> &parameters) {
    // This planner-only API does not initialize an executor or schedule side effects.
    auto execute = duckdb::make_uniq<duckdb::ExecuteStatement>();
    execute->name = duckdb::Identifier(statement.prepared->GetName());
    execute->query = statement.sql;
    for (std::size_t i = 0; i < parameters.size(); ++i)
        execute->bound_values[duckdb::Identifier(std::to_string(i + 1))] = duckdb::BoundParameterData(parameters[i]);
    return connection.context->BindStatement(std::move(execute));
}
void PgWireServer::Impl::Session::Describe(Reader &in) {
    auto kind = in.Byte(); auto name = in.String(); in.End();
    std::shared_ptr<Statement> statement; Portal *portal = nullptr;
    if (kind == 'S') {
        auto found = statements.find(name); if (found == statements.end()) throw WireError("26000", "Unknown prepared statement");
        statement = found->second;
        Buffer parameters; Put16(parameters, static_cast<std::uint16_t>(statement->parameter_oids.size()));
        for (auto oid : statement->parameter_oids) Put32(parameters, oid);
        Message(socket, 't', parameters);
    } else if (kind == 'P') {
        auto found = portals.find(name); if (found == portals.end()) throw WireError("34000", "Unknown portal");
        portal = &found->second; statement = portal->statement;
    } else throw WireError("08P01", "Invalid Describe object type", true);
    if (statement->control.kind == FdwKind::FETCH) {
        GuardTransaction(statement->sql);
        auto cursor = sql_cursors.find(statement->control.name);
        if (cursor == sql_cursors.end()) throw WireError("34000", "Unknown SQL cursor");
        RowDescription(cursor->second.result->GetNames(), cursor->second.result->GetTypes(), portal ? portal->result_formats : std::vector<std::uint16_t>{}, portal);
        return;
    }
    if (!statement->fdw_cursor_name.empty() || statement->control.kind != FdwKind::NONE || statement->empty || statement->setting || statement->deallocate) {
        Message(socket, 'n'); return;
    }
    EnsurePrepared(*statement);
    if (portal && portal->signature) {
        if (portal->signature->properties.return_type != duckdb::StatementReturnType::QUERY_RESULT) { Message(socket, 'n'); return; }
        RowDescription(portal->signature->names, portal->signature->types, portal->result_formats, portal);
    } else {
        auto names = statement->prepared->GetNames(); auto types = statement->prepared->GetTypes();
        auto properties = statement->prepared->GetStatementProperties();
        if (!properties.bound_all_parameters) {
            duckdb::vector<Value> parameters;
            for (auto oid : statement->parameter_oids) {
                if (!oid) throw WireError("0A000", "Unresolved result schema requires a Describe of a bound portal");
                parameters.emplace_back(DuckType(oid));
            }
            try {
                auto signature = BoundSignature(*statement, parameters);
                names = std::move(signature.names); types = std::move(signature.types); properties = signature.properties;
            } catch (const std::exception &) {
                throw WireError("0A000", "Dynamic result schema requires a Describe of a bound portal");
            }
        }
        statement->returns_rows = properties.return_type == duckdb::StatementReturnType::QUERY_RESULT;
        if (statement->returns_rows && types.empty()) throw WireError("0A000", "Dynamic result schema requires a Describe of a bound portal");
        for (const auto &type : types)
            if (type.id() == LogicalTypeId::UNKNOWN || type.id() == LogicalTypeId::INVALID || type.id() == LogicalTypeId::ANY)
                throw WireError("0A000", "Unresolved result type requires a Describe of a bound portal");
        statement->described_oids.clear(); statement->described = true;
        if (!statement->returns_rows) { Message(socket, 'n'); return; }
        for (const auto &type : types) statement->described_oids.push_back(PGType(type).oid);
        RowDescription(names, types, {}, nullptr);
    }
}
void PgWireServer::Impl::Session::Execute(Reader &in) {
    auto name = in.String(); auto maximum = in.U32(); in.End();
    auto found = portals.find(name); if (found == portals.end()) throw WireError("34000", "Unknown portal");
    auto &portal = found->second; auto &statement = *portal.statement; GuardTransaction(statement.sql);
    if (statement.control.kind != FdwKind::NONE) {
        FdwExecute(portal, maximum, false);
        if (statement.control.kind == FdwKind::ROLLBACK_TO) {
            const auto generation = savepoints.back().generation;
            for (auto entry = portals.begin(); entry != portals.end();) {
                if (entry->first != name && entry->second.generation > generation) entry = portals.erase(entry); else ++entry;
            }
        } else if (statement.control.kind == FdwKind::CLOSE_ALL) {
            for (auto entry = portals.begin(); entry != portals.end();) {
                if (entry->first != name) entry = portals.erase(entry); else ++entry;
            }
        }
        return;
    }
    if (statement.empty) { Message(socket, 'I'); portal.complete = true; return; }
    if (statement.deallocate) {
        Buffer body; PutString(body, "DEALLOCATE"); Message(socket, 'C', body);
        portals.clear(); statements.clear(); return;
    }
    if (statement.setting) { Buffer body; PutString(body, "SET"); Message(socket, 'C', body); portal.complete = true; return; }
    if (failed_transaction && TransactionEnd(statement.sql)) {
        connection.Rollback(); failed_transaction = false; Buffer body; PutString(body, "ROLLBACK"); Message(socket, 'C', body);
        TransactionFinished();
        portal.complete = true; return;
    }
    if (!statement.fdw_cursor_name.empty()) {
        if(portal.complete)throw WireError("55000","DECLARE portal has already executed");
        Portal cursor;cursor.statement=portal.statement;cursor.parameters=portal.parameters;FdwStore(std::move(cursor));portal.complete=true;return;
    }
    if (!portal.result) {
        ExecutionGuard execution(*this);
        if (connection.IsAutoCommit() && statement.prepared->GetStatementType() != duckdb::StatementType::TRANSACTION_STATEMENT) {
            connection.BeginTransaction(); implicit_extended_transaction = true;
        }
        ReadOnlyTransaction();
        if (server.protected_mode && read_only) { auto signature = BoundSignature(statement, portal.parameters); AuthorizeBound(signature.properties); }
        execution.StartTimer(); portal.result = statement.prepared->Execute(portal.parameters, false);
        if (portal.result->HasError()) NativeError(portal.result->GetErrorObject());
        if (portal.described) {
            auto &types = portal.result->GetTypes();
            if (types.size() != portal.described_oids.size()) throw WireError("0A000", "Bound result column count differs from Describe");
            for (std::size_t i = 0; i < types.size(); ++i)
                if (PGType(types[i]).oid != portal.described_oids[i]) throw WireError("0A000", "Unresolved parameter changes result type; specify an explicit SQL cast");
        }
    }
    Rows(portal, maximum);
    if (TransactionEnd(statement.sql)) TransactionFinished();
    else ReadOnlyTransaction();
    if (connection.IsAutoCommit()) { failed_transaction = false; implicit_extended_transaction = false; }
}
void PgWireServer::Impl::Session::Rows(Portal &portal, std::uint32_t maximum) {
    const bool data = portal.result->GetStatementProperties().return_type == duckdb::StatementReturnType::QUERY_RESULT;
    std::uint32_t emitted = 0;
    while (!portal.complete) {
        if (!portal.chunk || portal.chunk_row >= portal.chunk->size()) {
            portal.chunk = portal.result->Fetch(); portal.chunk_row = 0;
            if (portal.result->HasError()) NativeError(portal.result->GetErrorObject());
            if (!portal.chunk || !portal.chunk->size()) { portal.complete = true; break; }
        }
        if (data && maximum && emitted == maximum) { Message(socket, 's'); return; }
        if (data) {
            auto columns = portal.result->ColumnCount(); Buffer body; Put16(body, static_cast<std::uint16_t>(columns));
            for (std::size_t i = 0; i < columns; ++i) {
                auto value = portal.chunk->GetValue(i, portal.chunk_row);
                if (value.IsNull()) { Put32(body, UINT32_MAX); continue; }
                auto field = Field(value, Format(portal.result_formats, i, columns));
                if (field.size() > MAX_FRAME - 4 || body.size() + field.size() + 4 > MAX_FRAME - 4)
                    throw WireError("54000", "Result row exceeds PGwire frame limit");
                Put32(body, static_cast<std::uint32_t>(field.size())); body.insert(body.end(), field.begin(), field.end());
            }
            Message(socket, 'D', body); ++portal.sent; ++emitted;
        } else if (portal.result->GetStatementProperties().return_type == duckdb::StatementReturnType::CHANGED_ROWS) {
            portal.changed += portal.chunk->GetValue(0, portal.chunk_row).GetValue<std::uint64_t>();
        }
        ++portal.chunk_row;
    }
    if (portal.tag.empty()) portal.tag = CommandTag(*portal.statement, *portal.result, data ? portal.sent : portal.changed);
    Buffer complete; PutString(complete, portal.tag); Message(socket, 'C', complete);
}

void PgWireServer::Impl::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_lock);
    if (!stopping.load()) throw std::runtime_error("PGwire server is already running");
    if (!max_active_queries || max_active_queries > MAX_OBJECTS || !query_timeout_ms || query_timeout_ms > 86400000)
        throw std::invalid_argument("PGwire requires query capacity 1..256 and timeout 1..86400000 milliseconds");
    if (!protected_mode) {
        if (host != "127.0.0.1" && host != "localhost") throw std::invalid_argument("Legacy PGwire requires IPv4 loopback");
        if (password.empty() || password.size() > 1024 || password.find('\0') != std::string::npos)
            throw std::invalid_argument("PGwire requires a nonempty password of at most 1024 bytes");
    } else {
        duckdb::Connection check(database);
        const auto require_boolean = [&](const char *name, bool expected) {
            Value value;
            if (!check.context->TryGetCurrentSetting(duckdb::Identifier(name), value) || value.IsNull() || value.GetValue<bool>() != expected)
                throw std::invalid_argument("Protected PGwire requires disabled external access, locked configuration and signed extensions");
        };
        require_boolean("enable_external_access", false); require_boolean("lock_configuration", true);
        require_boolean("allow_unsigned_extensions", false);
        const auto &native_config = duckdb::DBConfig::GetConfig(*check.context);
        const auto &config = native_config.options;
        // DuckDB itself grants file access to its storage, WAL and spill files
        // when external access is disabled. No additional caller exceptions are
        // admitted, and protected SQL cannot invoke file-reading functions.
        std::set<std::string> storage_paths, storage_directories;
        if (!duckdb::DBConfig::IsInMemoryDatabase(config.database_path.c_str())) {
            for (const auto suffix : {"", ".wal", ".wal.checkpoint", ".wal.recovery"})
                storage_paths.insert(native_config.SanitizeAllowedPath(config.database_path + suffix));
            storage_directories.insert(native_config.SanitizeAllowedPath(config.database_path + ".tmp") + "/");
            storage_directories.insert(native_config.SanitizeAllowedPath((std::filesystem::path(config.database_path).parent_path() / "native-spill").string()) + "/");
        } else storage_directories.insert(native_config.SanitizeAllowedPath(".tmp") + "/");
        if (!config.allowed_configs.empty()) throw std::invalid_argument("Protected PGwire requires no configurable-setting exceptions");
        for (const auto &path : config.allowed_paths) if (!storage_paths.count(path))
            throw std::invalid_argument("Protected PGwire permits only the published database and its native WAL files");
        for (const auto &path : config.allowed_directories) if (!storage_directories.count(path))
            throw std::invalid_argument("Protected PGwire permits only the published database's native spill directory");
        if (options.tls_cert.empty() || options.tls_key.empty() || options.users.empty() || options.users.size() > 64)
            throw std::invalid_argument("Protected PGwire requires TLS certificate, key and configured users");
        const auto valid_name = [](const std::string &name) {
            if (name.empty() || name.size() > 63) return false;
            const auto first = static_cast<unsigned char>(name.front());
            if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9') || first == '_')) return false;
            return std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            });
        };
        if (!valid_name(options.database_name)) throw std::invalid_argument("Invalid published database name");
        std::set<std::string> names;
        for (const auto &user : options.users) {
            if (!valid_name(user.name) || !names.insert(user.name).second) throw std::invalid_argument("Invalid or duplicate PGwire user");
            ValidateScramVerifier(user.scram_verifier);
        }
        unsigned char random[32];
        if (RAND_bytes(random, sizeof(random)) != 1) throw std::runtime_error("Cannot generate authentication state");
        std::string fake_password; const char hex[] = "0123456789abcdef";
        for (auto byte : random) { fake_password += hex[byte >> 4]; fake_password += hex[byte & 15]; }
        fake_verifier = MakeScramVerifier(fake_password);
        if (tls_context) SSL_CTX_free(tls_context);
        tls_context = SSL_CTX_new(TLS_server_method());
        if (!tls_context || SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION) != 1 ||
            SSL_CTX_use_certificate_chain_file(tls_context, options.tls_cert.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(tls_context, options.tls_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(tls_context) != 1) throw std::runtime_error("Invalid TLS certificate or private key");
        SSL_CTX_set_options(tls_context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("Cannot create PGwire listener");
    const auto flags = fcntl(listener, F_GETFL, 0);
    if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK)) {
        close(listener); listener = -1; throw std::runtime_error("Cannot configure PGwire listener");
    }
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address {}; address.sin_family = AF_INET;
    if (host == "localhost") address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close(listener); listener = -1; throw std::invalid_argument("PGwire bind host must be an IPv4 literal or localhost");
    }
    address.sin_port = htons(requested_port);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 128)) {
        close(listener); listener = -1; throw std::runtime_error("Cannot bind PGwire listener");
    }
    socklen_t size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size)) {
        close(listener); listener = -1; throw std::runtime_error("Cannot read PGwire bound port");
    }
    bound_port.store(ntohs(address.sin_port)); stopping.store(false);
    try { accept_thread = std::thread([this] { Accept(); }); }
    catch (...) { stopping.store(true); close(listener); listener = -1; bound_port.store(0); throw; }
}
void PgWireServer::Impl::Accept() {
    while (!stopping.load()) {
        pollfd pending {listener, POLLIN, 0};
        const auto ready = poll(&pending, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (!ready) continue;
        if (stopping.load() || (pending.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!(pending.revents & POLLIN)) continue;
        auto client = accept(listener, nullptr, nullptr);
        if (client < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
        if (stopping.load()) { close(client); break; }
        // macOS inherits O_NONBLOCK from the listener; protocol sessions read
        // complete frames on their worker thread with blocking sockets.
        const auto client_flags = fcntl(client, F_GETFL, 0);
        if (client_flags < 0 || fcntl(client, F_SETFL, client_flags & ~O_NONBLOCK)) {
            close(client); continue;
        }
        // Send small protocol responses without waiting for TCP coalescing.
        const int no_delay = 1;
        if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay))) {
            close(client); continue;
        }
        timeval timeout {10, 0}; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        timeout.tv_sec = 10; setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int no_signal = 1; setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
        bool socket_transferred = false;
        try {
            for (auto job = jobs.begin(); job != jobs.end();) {
                if (job->done->load()) { job->thread.join(); job = jobs.erase(job); } else ++job;
            }
            auto id = next_id.fetch_add(1);
            auto session = std::make_shared<Session>(*this, client, id);
            socket_transferred = true;
            {
                std::lock_guard<std::mutex> guard(sessions_lock);
                if (sessions.size() >= MAX_OBJECTS) { session->CloseSocket(); continue; }
                sessions[id] = session;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            try {
                jobs.emplace_back();
                jobs.back().done = done;
                jobs.back().thread = std::thread([this, session, done] {
                    session->Run();
                    { std::lock_guard<std::mutex> guard(sessions_lock); sessions.erase(session->id); }
                    done->store(true);
                });
            } catch (...) {
                if (!jobs.empty() && !jobs.back().thread.joinable()) jobs.pop_back();
                std::lock_guard<std::mutex> guard(sessions_lock); sessions.erase(id); session->CloseSocket(); throw;
            }
        } catch (...) { if (!socket_transferred) close(client); }
    }
}
void PgWireServer::Impl::Cancel(std::uint32_t id, std::uint32_t secret) {
    std::shared_ptr<Session> target;
    { std::lock_guard<std::mutex> guard(sessions_lock); auto found = sessions.find(id);
      if (found != sessions.end() && found->second->cancel_secret == secret) target = found->second; }
    if (target) target->Interrupt();
}
void PgWireServer::Impl::Stop() noexcept {
    std::lock_guard<std::mutex> lifecycle(lifecycle_lock);
    if (stopping.exchange(true)) return;
    parse_wake.notify_all();
    if (listener >= 0) shutdown(listener, SHUT_RDWR);
    std::vector<std::shared_ptr<Session>> active;
    { std::lock_guard<std::mutex> guard(sessions_lock); for (auto &item : sessions) active.push_back(item.second); }
    for (auto &session : active) { try { session->Shutdown(); } catch (...) {} }
    if (accept_thread.joinable()) accept_thread.join();
    // Include clients accepted immediately before Stop took its first snapshot.
    { std::lock_guard<std::mutex> guard(sessions_lock); active.clear(); for (auto &item : sessions) active.push_back(item.second); }
    for (auto &session : active) { try { session->Shutdown(); } catch (...) {} }
    for (auto &job : jobs) if (job.thread.joinable()) job.thread.join();
    jobs.clear();
    insert_template.reset();
    for(auto &entry:select_templates)entry.reset();
    select_nodes=select_strings=0;
    if (listener >= 0) { close(listener); listener = -1; }
    bound_port.store(0);
}
PgWireServer::PgWireServer(duckdb::DuckDB &database, std::string bind_host, std::uint16_t port, std::string password,
                           std::size_t max_active_queries, std::uint32_t query_timeout_ms)
    : impl_(new Impl(database, std::move(bind_host), port, std::move(password), max_active_queries, query_timeout_ms)) {}
PgWireServer::PgWireServer(duckdb::DuckDB &database, PgWireOptions options)
    : impl_(new Impl(database, std::move(options))) {}
PgWireServer::~PgWireServer() { Stop(); }
void PgWireServer::Start() { impl_->Start(); }
void PgWireServer::Stop() noexcept { impl_->Stop(); }
std::uint16_t PgWireServer::Port() const noexcept { return impl_->bound_port.load(); }
std::string PgWireServer::Endpoint() const { return "postgresql://" + impl_->host + ":" + std::to_string(Port()); }
} // namespace duckonwire
