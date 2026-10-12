#include "duckonwire/access.hpp"
#include "duckonwire/json.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
const std::string VERIFIER = "SCRAM-SHA-256$4096:c2FsdA==$"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
struct Fixture {
    explicit Fixture(const fs::path &parent) {
        fs::create_directories(parent);
        auto text = (parent / "duckonwire-access-XXXXXX").string();
        std::vector<char> name(text.begin(), text.end()); name.push_back(0);
        auto created = mkdtemp(name.data());
        Require(created != nullptr, "Cannot create owned access fixture");
        path = created;
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
    fs::path Write(const std::string &content, mode_t mode = 0600) {
        auto file = path / ("access-" + std::to_string(next++) + ".tsv");
        std::ofstream output(file, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size())); output.close();
        Require(bool(output) && chmod(file.c_str(), mode) == 0, "Cannot prepare owned access fixture");
        return file;
    }
    fs::path path;
    unsigned next = 0;
};
std::string Config(const std::string &name = "reader", const std::string &role = "read") {
    return "database\tduckonwire\nuser\t" + name + "\t" + role + "\t" + VERIFIER + "\n";
}
void Rejected(const std::string &path) {
    try { duckonwire::ReadAccessConfig(path); }
    catch (const std::runtime_error &error) {
        Require(std::string(error.what()) == "Invalid access configuration", "Configuration error exposed input details");
        return;
    }
    throw std::runtime_error("Unsafe access configuration was accepted");
}
void RejectBounded(const fs::path &path) {
    const auto child = fork();
    Require(child >= 0, "Cannot fork bounded access probe");
    if (!child) {
        try { Rejected(path.string()); _exit(0); } catch (...) { _exit(1); }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        int status = 0;
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Nonregular input was not rejected");
            return;
        }
        if (result < 0 || std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL); waitpid(child, &status, 0);
            throw std::runtime_error("Access reader blocked on nonregular input");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
int AvailableDescriptor() {
    const auto descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
    Require(descriptor >= 0, "Cannot measure descriptor ownership"); close(descriptor); return descriptor;
}
} // namespace

int main(int argc, char **argv) {
    fs::path parent = fs::temp_directory_path();
    if (argc == 3 && std::string(argv[1]) == "--fixture-parent") parent = argv[2];
    else if (argc != 1) { std::cerr << "Usage: access-tests [--fixture-parent DIRECTORY]\n"; return 2; }
    std::vector<std::pair<std::string, std::string>> checks;
    try {
        Fixture fixture(parent);
        auto test = [&](const std::string &name, const std::function<void()> &body) {
            try { body(); checks.emplace_back(name, ""); }
            catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
        };
        test("private_lf_config_preserves_database_verifier_and_roles", [&] {
            auto path = fixture.Write(Config("reader.name") + "user\tWriter_2-test\twrite\t" + VERIFIER + "\n");
            auto config = duckonwire::ReadAccessConfig(path.string());
            Require(config.database == "duckonwire" && config.users.size() == 2, "Valid configuration shape changed");
            Require(config.users[0].name == "reader.name" && config.users[0].read_only && config.users[0].verifier == VERIFIER,
                    "Reader rule or verifier changed");
            Require(config.users[1].name == "Writer_2-test" && !config.users[1].read_only, "Writer rule changed");
            Require(config.databases == std::vector<std::string>{"duckonwire"} &&
                    config.users[0].database == "duckonwire" && config.users[1].database == "duckonwire",
                    "Legacy rules were not scoped to their database");
        });
        test("owner_read_only_crlf_and_final_line_without_newline", [&] {
            auto path = fixture.Write("database\tduckonwire\r\nuser\treader\tread\t" + VERIFIER, 0400);
            Require(duckonwire::ReadAccessConfig(path.string()).users[0].read_only, "CRLF/0400 configuration failed");
        });
        test("missing_unknown_extra_and_empty_records_fail_closed", [&] {
            for (const auto &content : std::vector<std::string>{"", "database\tduckonwire\n", "user\treader\tread\t" + VERIFIER,
                 "database\tduckonwire\textra\nuser\treader\tread\t" + VERIFIER, Config() + "\n",
                 Config() + "database\tother\n", Config() + "unknown\treader\tread\t" + VERIFIER,
                 "database\tduckonwire\nuser\treader\tread\t" + VERIFIER + "\textra\n",
                 "database\tduckonwire\n# comments are not records\n"}) Rejected(fixture.Write(content).string());
        });
        test("strict_roles_and_verifier_prefix_fail_closed", [&] {
            for (const auto &role : {"READ", "admin", "", "read write"}) Rejected(fixture.Write(Config("reader", role)).string());
            for (const auto &verifier : {"", "REPLACE_WITH_GENERATED_SCRAM_VERIFIER", "SCRAM-SHA-256$", "SCRAM-SHA-256$not printable"})
                Rejected(fixture.Write("database\tduckonwire\nuser\treader\tread\t" + std::string(verifier) + "\n").string());
        });
        test("database_and_user_names_are_ascii_and_bounded", [&] {
            const std::string longest(63, 'a');
            Require(duckonwire::ReadAccessConfig(fixture.Write("database\t" + longest + "\nuser\t" + longest + "\tread\t" + VERIFIER).string()).users.size() == 1,
                    "Valid 63-character names failed");
            for (const auto &name : std::vector<std::string>{"", "-reader", ".reader", "reader/name", "reader name", "caf\xc3\xa9", std::string(64, 'a')}) {
                Rejected(fixture.Write(Config(name)).string());
                Rejected(fixture.Write("database\t" + name + "\nuser\treader\tread\t" + VERIFIER).string());
            }
        });
        test("duplicate_users_rejected_and_case_sensitive_names_preserved", [&] {
            Rejected(fixture.Write(Config() + "user\treader\twrite\t" + VERIFIER).string());
            Require(duckonwire::ReadAccessConfig(fixture.Write(Config() + "user\tReader\twrite\t" + VERIFIER).string()).users.size() == 2,
                    "Distinct case-sensitive users collapsed");
        });
        test("multiple_databases_scope_roles_and_separate_credentials", [&] {
            auto other_verifier = VERIFIER;
            other_verifier.replace(std::string("SCRAM-SHA-256$").size(), 4, "8192");
            auto path = fixture.Write("database\tPrimaryDB\r\nuser\tshared\tread\t" + VERIFIER +
                                      "\r\nuser\twriter\twrite\t" + VERIFIER +
                                      "\r\ndatabase\tanalytics.2\r\nuser\tshared\twrite\t" + other_verifier);
            const auto config = duckonwire::ReadAccessConfig(path.string());
            Require(config.database == "PrimaryDB" &&
                    config.databases == std::vector<std::string>{"PrimaryDB", "analytics.2"} && config.users.size() == 3,
                    "Multiple database order or default changed");
            Require(config.users[0].database == "PrimaryDB" && config.users[0].name == "shared" &&
                    config.users[0].read_only && config.users[0].verifier == VERIFIER,
                    "First database reader grant changed");
            Require(config.users[1].database == "PrimaryDB" && config.users[1].name == "writer" && !config.users[1].read_only,
                    "First database writer grant changed");
            Require(config.users[2].database == "analytics.2" && config.users[2].name == "shared" &&
                    !config.users[2].read_only && config.users[2].verifier == other_verifier,
                    "Second database grant inherited another database's credential or role");
        });
        test("each_database_section_requires_at_least_one_user", [&] {
            for (const auto &content : std::vector<std::string>{
                "database\tfirst\ndatabase\tsecond\nuser\treader\tread\t" + VERIFIER,
                Config() + "database\tsecond\ndatabase\tthird\nuser\treader\tread\t" + VERIFIER,
                Config() + "database\tsecond",
                "user\treader\tread\t" + VERIFIER + "\ndatabase\tfirst\nuser\treader\tread\t" + VERIFIER})
                Rejected(fixture.Write(content).string());
        });
        test("database_alias_duplicates_and_reserved_aliases_are_rejected", [&] {
            for (const auto &name : {"duckonwire", "DuckOnWire", "DUCKONWIRE"})
                Rejected(fixture.Write(Config() + "database\t" + name + "\nuser\tother\tread\t" + VERIFIER).string());
            for (const auto &name : {"system", "SYSTEM", "System", "temp", "TEMP", "Temp"}) {
                Rejected(fixture.Write("database\t" + std::string(name) + "\nuser\treader\tread\t" + VERIFIER).string());
                Rejected(fixture.Write(Config() + "database\t" + name + "\nuser\treader\tread\t" + VERIFIER).string());
            }
        });
        test("duplicate_users_are_rejected_within_every_database", [&] {
            const auto second = Config() + "database\tsecond\nuser\treader\twrite\t" + VERIFIER + "\n";
            Rejected(fixture.Write(second + "user\treader\tread\t" + VERIFIER).string());
            const auto config = duckonwire::ReadAccessConfig(fixture.Write(second + "user\tReader\tread\t" + VERIFIER).string());
            Require(config.users.size() == 3 && config.users[1].name == "reader" && config.users[2].name == "Reader" &&
                    config.users[1].database == "second" && config.users[2].database == "second",
                    "Second database lost case-sensitive username handling");
        });
        test("database_count_32_allowed_33_rejected", [&] {
            std::string content;
            for (int i = 0; i < 32; ++i)
                content += "database\tdb" + std::to_string(i) + "\nuser\tshared\tread\t" + VERIFIER + "\n";
            const auto config = duckonwire::ReadAccessConfig(fixture.Write(content).string());
            Require(config.databases.size() == 32 && config.users.size() == 32 && config.database == "db0" &&
                    config.users.back().database == "db31", "32-database boundary or grant scopes failed");
            Rejected(fixture.Write(content + "database\tdb32\nuser\tshared\tread\t" + VERIFIER).string());
        });
        test("grant_count_64_is_global_across_database_sections", [&] {
            std::string content;
            for (int i = 0; i < 32; ++i)
                content += "database\tdb" + std::to_string(i) + "\nuser\treader\tread\t" + VERIFIER +
                           "\nuser\twriter\twrite\t" + VERIFIER + "\n";
            const auto config = duckonwire::ReadAccessConfig(fixture.Write(content).string());
            Require(config.users.size() == 64 && config.databases.size() == 32,
                    "64 scoped grants across databases were rejected");
            Rejected(fixture.Write(content + "user\tother\tread\t" + VERIFIER).string());
        });
        test("later_sections_preserve_strict_names_roles_and_verifiers", [&] {
            for (const auto &name : std::vector<std::string>{"", "-bad", "bad/name", "bad name", "caf\xc3\xa9", std::string(64, 'a')}) {
                Rejected(fixture.Write(Config() + "database\t" + name + "\nuser\treader\tread\t" + VERIFIER).string());
                Rejected(fixture.Write(Config() + "database\tsecond\nuser\t" + name + "\tread\t" + VERIFIER).string());
            }
            Rejected(fixture.Write(Config() + "database\tsecond\nuser\treader\tadmin\t" + VERIFIER).string());
            Rejected(fixture.Write(Config() + "database\tsecond\nuser\treader\tread\tPRIVATE_INVALID_VERIFIER").string());
            Rejected(fixture.Write(Config() + "database\tsecond\textra\nuser\treader\tread\t" + VERIFIER).string());
        });
        test("user_count_64_allowed_65_rejected", [&] {
            std::string content = "database\tduckonwire\n";
            for (int i = 0; i < 64; ++i) content += "user\tuser" + std::to_string(i) + "\tread\t" + VERIFIER + "\n";
            Require(duckonwire::ReadAccessConfig(fixture.Write(content).string()).users.size() == 64, "64-user boundary failed");
            Rejected(fixture.Write(content + "user\tuser64\tread\t" + VERIFIER).string());
        });
        test("group_world_special_and_executable_permissions_rejected", [&] {
            for (mode_t mode : {0644, 0640, 0660, 0666, 0404, 0700, 0000, 04600, 02600})
                Rejected(fixture.Write(Config(), mode).string());
        });
        test("symlink_missing_file_and_directory_rejected", [&] {
            auto target = fixture.Write(Config()); auto link = fixture.path / "linked.tsv";
            fs::create_symlink(target, link); Rejected(link.string());
            fs::create_symlink(fixture.path / "missing.tsv", fixture.path / "dangling.tsv");
            Rejected((fixture.path / "dangling.tsv").string()); Rejected((fixture.path / "missing.tsv").string());
            Rejected(fixture.path.string());
            Require(duckonwire::ReadAccessConfig(target.string()).users.size() == 1, "Symlink rejection changed its target");
        });
        test("fifo_is_rejected_with_external_deadline", [&] {
            auto path = fixture.path / "access.fifo";
            Require(mkfifo(path.c_str(), 0600) == 0, "Cannot prepare owned FIFO"); RejectBounded(path);
        });
        test("file_size_limit_64_kib_inclusive", [&] {
            auto content = Config(); content.pop_back(); content.resize(64 * 1024, 'A');
            Require(duckonwire::ReadAccessConfig(fixture.Write(content).string()).users.size() == 1, "64-KiB boundary failed");
            content.push_back('A'); Rejected(fixture.Write(content).string());
        });
        test("truncated_columns_nul_and_control_bytes_rejected", [&] {
            for (const auto &content : std::vector<std::string>{"database\t", "database\tduckonwire\nuser\treader\tread", "database\tduckonwire\nuser\treader\tread\t"})
                Rejected(fixture.Write(content).string());
            for (char byte : {'\0', '\r', '\x1f', '\x7f', static_cast<char>(0xff)}) {
                auto content = Config(); content.insert(content.size() - 2, 1, byte); Rejected(fixture.Write(content).string());
            }
            auto path = fixture.Write(Config()).string(); path.push_back(0); path += "ignored-suffix"; Rejected(path);
        });
        test("errors_do_not_echo_config_or_path_and_descriptors_are_closed", [&] {
            auto valid = fixture.Write(Config()); auto invalid = fixture.Write(Config("TEST_ONLY_PRIVATE_MARKER", "invalid"));
            auto denied = fixture.Write(Config(), 0644); const auto before = AvailableDescriptor();
            for (int i = 0; i < 64; ++i) {
                Require(duckonwire::ReadAccessConfig(valid.string()).users.size() == 1, "Repeated valid read failed");
                Rejected(invalid.string()); Rejected(denied.string());
            }
            Require(AvailableDescriptor() == before, "Access reader leaked descriptors");
        });
    } catch (const std::exception &error) { checks.emplace_back("owned_fixture", error.what()); }
    bool passed = !checks.empty();
    std::cout << "{\"checks\":[";
    for (std::size_t i = 0; i < checks.size(); ++i) {
        if (i) std::cout << ',';
        const auto &check = checks[i]; passed = passed && check.second.empty();
        std::cout << "{\"name\":" << duckonwire::JsonString(check.first) << ",\"passed\":" << (check.second.empty() ? "true" : "false");
        if (!check.second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(check.second);
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
