#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace duckdb { class DuckDB; class Connection; }
namespace duckonwire {

// A pointer names an immutable, closed DuckDB file. Publication never replaces
// the host instance, its security policy, or a client's connection.
class ReadCopyController {
public:
    enum class Result { Unchanged, Published, Rejected, Restored, Unavailable, VersionLimit };
    using PathCheck = std::function<void(const std::filesystem::path &)>;
    // Fault hooks are for owned C++ fixtures, never exposed through SQL or CLI.
    using AfterPublish = std::function<void(duckdb::Connection &)>;

    ReadCopyController(duckdb::DuckDB &host, std::filesystem::path root,
                       std::filesystem::path pointer, PathCheck path_check = {},
                       AfterPublish after_publish = {});
    ~ReadCopyController();
    ReadCopyController(const ReadCopyController &) = delete;
    ReadCopyController &operator=(const ReadCopyController &) = delete;

    Result Poll();
    const std::filesystem::path &Path() const;
    const std::filesystem::path &Pointer() const;
    const std::filesystem::path &StorageDirectory() const;
    std::uint64_t Generation() const;
    std::shared_ptr<std::atomic<bool>> Available() const;

    // Performs owner-only pointer/file checks before configuring a new host.
    static std::filesystem::path InitialPath(const std::filesystem::path &root,
                                             const std::filesystem::path &pointer);
    static std::filesystem::path VersionDirectory(const std::filesystem::path &root,
                                                  const std::filesystem::path &pointer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
