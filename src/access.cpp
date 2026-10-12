#include "duckonwire/access.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckonwire {
namespace {
constexpr std::size_t MAX_FILE_BYTES = 64 * 1024;
constexpr std::size_t MAX_USERS = 64;
constexpr std::size_t MAX_DATABASES = 32;
constexpr std::string_view SCRAM_PREFIX = "SCRAM-SHA-256$";

[[noreturn]] void Invalid() {
    // A verifier, a password, or an attacker-controlled path must never be echoed.
    throw std::runtime_error("Invalid access configuration");
}

struct File {
    explicit File(int descriptor) : descriptor(descriptor) {}
    ~File() { if (descriptor >= 0) close(descriptor); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int descriptor;
};

bool Name(std::string_view value) {
    if (value.empty() || value.size() > 63) return false;
    auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    if (!alnum(static_cast<unsigned char>(value.front()))) return false;
    for (unsigned char c : value) if (!alnum(c) && c != '-' && c != '.') return false;
    return true;
}

std::string DatabaseKey(std::string_view value) {
    std::string result(value);
    // Database aliases are case-insensitive in DuckDB, independently of locale.
    for (char &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

std::vector<std::string_view> Fields(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    for (;;) {
        const auto end = line.find('\t', begin);
        result.push_back(line.substr(begin, end == std::string_view::npos ? end : end - begin));
        if (end == std::string_view::npos) return result;
        begin = end + 1;
        if (result.size() > 4) Invalid();
    }
}

bool Verifier(std::string_view value) {
    if (value.size() <= SCRAM_PREFIX.size() || value.substr(0, SCRAM_PREFIX.size()) != SCRAM_PREFIX)
        return false;
    for (unsigned char c : value) if (c < 0x21 || c > 0x7e) return false;
    return true;
}
} // namespace

AccessConfig ReadAccessConfig(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos) Invalid();
    // O_NONBLOCK avoids hanging on a FIFO before fstat can reject it.
    File file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.descriptor < 0) Invalid();
    struct stat metadata {};
    if (fstat(file.descriptor, &metadata) || !S_ISREG(metadata.st_mode) ||
        ((metadata.st_mode & 07777) != 0400 && (metadata.st_mode & 07777) != 0600) ||
        metadata.st_size < 0 || static_cast<std::uintmax_t>(metadata.st_size) > MAX_FILE_BYTES)
        Invalid();

    std::string content;
    std::array<char, 4096> buffer {};
    for (;;) {
        const auto count = read(file.descriptor, buffer.data(), buffer.size());
        if (count < 0) { if (errno == EINTR) continue; Invalid(); }
        if (!count) break;
        if (static_cast<std::size_t>(count) > MAX_FILE_BYTES - content.size()) Invalid();
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }

    AccessConfig config;
    std::unordered_set<std::string> database_names, user_names;
    std::size_t begin = 0;
    while (begin < content.size()) {
        const auto newline = content.find('\n', begin);
        const auto end = newline == std::string::npos ? content.size() : newline;
        std::string_view line(content.data() + begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) Invalid();
        const auto fields = Fields(line);
        if (fields[0] == "database") {
            if (fields.size() != 2 || !Name(fields[1]) || config.databases.size() >= MAX_DATABASES ||
                (!config.databases.empty() && user_names.empty())) Invalid();
            auto key = DatabaseKey(fields[1]);
            if (key == "system" || key == "temp" || !database_names.insert(std::move(key)).second) Invalid();
            config.databases.emplace_back(fields[1]);
            if (config.database.empty()) config.database = config.databases.back();
            user_names.clear();
        } else {
            if (config.databases.empty() || fields.size() != 4 || fields[0] != "user" || !Name(fields[1]) ||
                (fields[2] != "read" && fields[2] != "write") || !Verifier(fields[3]) ||
                config.users.size() >= MAX_USERS)
                Invalid();
            std::string name(fields[1]);
            if (!user_names.insert(name).second) Invalid();
            config.users.push_back({std::move(name), std::string(fields[3]), fields[2] == "read", config.databases.back()});
        }
        begin = newline == std::string::npos ? content.size() : newline + 1;
    }
    if (config.databases.empty() || user_names.empty()) Invalid();
    return config;
}
} // namespace duckonwire
