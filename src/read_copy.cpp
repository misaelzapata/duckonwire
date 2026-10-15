#include "duckonwire/read_copy.hpp"
#include "duckonwire/native_access.hpp"
#include "duckdb.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

namespace duckonwire {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void Invalid() { throw std::runtime_error("Invalid private read-copy publication"); }

bool Within(const fs::path &path, const fs::path &root) {
    auto item = path.begin();
    for (auto base = root.begin(); base != root.end(); ++base, ++item)
        if (item == path.end() || *item != *base) return false;
    return item != path.end();
}

fs::path LocalPath(const fs::path &root, const fs::path &input) {
    auto path = (input.is_absolute() ? input : root / input).lexically_normal();
    if (!Within(path, root)) Invalid();
    auto current = root;
    for (const auto &part : path.lexically_relative(root)) {
        current /= part;
        struct stat status {};
        if (lstat(current.c_str(), &status) != 0 || S_ISLNK(status.st_mode)) Invalid();
    }
    return path;
}

fs::path PointerPath(const fs::path &root, const fs::path &input) {
    const auto path = (input.is_absolute() ? input : root / input).lexically_normal();
    if (!Within(path, root)) Invalid();
    if (path.parent_path() != root) LocalPath(root, path.parent_path());
    return path;
}

fs::path ManifestPath(const fs::path &root, const fs::path &input) {
    const auto path = PointerPath(root, input);
    struct stat status {};
    if (lstat(path.c_str(), &status) == 0) {
        if (S_ISLNK(status.st_mode)) Invalid();
    } else if (errno != ENOENT) Invalid();
    return path;
}

struct File {
    int fd = -1;
    struct stat status {};
    File(const fs::path &path, bool pointer) {
        fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) Invalid();
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
            (pointer ? ((status.st_mode & 07777) != 0600 && (status.st_mode & 07777) != 0400)
                     : ((status.st_mode & 07777) != 0400 || status.st_nlink != 1))) {
            close(fd); fd = -1; Invalid();
        }
    }
    ~File() { if (fd >= 0) close(fd); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
};

std::string Fingerprint(const struct stat &s) {
#ifdef __APPLE__
    const auto modified = s.st_mtimespec, changed = s.st_ctimespec;
#else
    const auto modified = s.st_mtim, changed = s.st_ctim;
#endif
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" + std::to_string(s.st_size) + ":" +
           std::to_string(s.st_mode) + ":" + std::to_string(modified.tv_sec) + ":" + std::to_string(modified.tv_nsec) + ":" +
           std::to_string(changed.tv_sec) + ":" + std::to_string(changed.tv_nsec);
}

std::string PointerFingerprint(const fs::path &pointer) {
    struct stat status {};
    return lstat(pointer.c_str(), &status) == 0 ? Fingerprint(status) : "missing";
}

fs::path ReadPointer(const fs::path &root, const fs::path &pointer, const fs::path &manifest = {}) {
    File file(LocalPath(root, manifest.empty() ? pointer : manifest), true);
    if (file.status.st_size < 1 || file.status.st_size > 4096) Invalid();
    std::string value(static_cast<std::size_t>(file.status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        auto bytes = read(file.fd, value.data() + offset, value.size() - offset);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) Invalid();
        offset += static_cast<std::size_t>(bytes);
    }
    struct stat current {};
    if (fstat(file.fd, &current) != 0 || Fingerprint(current) != Fingerprint(file.status)) Invalid();
    if (value.back() == '\n') value.pop_back();
    if (value.empty()) Invalid();
    for (unsigned char c : value) if (c < 32 || c == 127) Invalid();
    fs::path selected(value);
    if (selected.is_absolute() || selected.filename() != selected || selected == ".." || selected == ".") Invalid();
    if (value.size() > 255) Invalid();
    for (unsigned char c : value)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '.' && c != '_' && c != '-') Invalid();
    const auto directory = ReadCopyController::VersionDirectory(root, pointer);
    return LocalPath(root, directory / selected);
}

bool HasAck(const fs::path &pointer) {
    struct stat status {};
    if (lstat((pointer.string() + ".active").c_str(), &status) == 0) return true;
    if (errno != ENOENT) Invalid();
    return false;
}

void SaveAck(const fs::path &root, const fs::path &pointer, const fs::path &candidate) {
    const auto ack = ManifestPath(root, pointer.string() + ".active");
    // Refuse to replace an unsafe or foreign acknowledgement.
    if (HasAck(pointer)) { File current(ack, true); }
    std::string temporary_name = ack.string() + ".update.XXXXXX";
    int fd = mkstemp(temporary_name.data());
    if (fd < 0) throw std::runtime_error("Cannot create read-copy acknowledgement");
    const fs::path temporary(temporary_name);
    bool renamed = false;
    try {
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0600) != 0)
            throw std::runtime_error("Cannot secure read-copy acknowledgement");
        const auto value = candidate.filename().string() + "\n";
        std::size_t offset = 0;
        while (offset < value.size()) {
            auto count = write(fd, value.data() + offset, value.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Cannot write read-copy acknowledgement");
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(fd) != 0) throw std::runtime_error("Cannot persist read-copy acknowledgement");
        close(fd); fd = -1;
        if (rename(temporary.c_str(), ack.c_str()) != 0) throw std::runtime_error("Cannot publish read-copy acknowledgement");
        renamed = true;
        const int directory = open(ack.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) throw std::runtime_error("Cannot open read-copy acknowledgement directory");
        const auto synced = fsync(directory); close(directory);
        if (synced != 0) throw std::runtime_error("Cannot persist read-copy acknowledgement directory");
    } catch (...) {
        if (fd >= 0) close(fd);
        if (!renamed) unlink(temporary.c_str());
        throw;
    }
}

std::string Quote(const std::string &value) {
    std::string result = "'";
    for (char c : value) { result += c; if (c == '\'') result += c; }
    return result + "'";
}

void Query(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    if (!result || result->HasError()) throw std::runtime_error("Read-copy catalog operation failed");
}

void CheckIdentity(const fs::path &path, const struct stat &expected) {
    File current(path, false);
    if (Fingerprint(current.status) != Fingerprint(expected)) Invalid();
    // A closed, checkpointed version has no WAL whose contents could change later.
    for (const auto suffix : {".wal", ".wal.checkpoint", ".wal.recovery"}) {
        struct stat status {};
        if (lstat((path.string() + suffix).c_str(), &status) == 0 || errno != ENOENT) Invalid();
    }
}

void ValidateVersion(const fs::path &path, const struct stat &expected) {
    try {
        CheckIdentity(path, expected);
        duckdb::DBConfig config;
        config.SetOptionByName(duckdb::Identifier("access_mode"), duckdb::Value("READ_ONLY"));
        config.SetOptionByName(duckdb::Identifier("enable_external_access"), duckdb::Value(false));
        config.SetOptionByName(duckdb::Identifier("allow_unsigned_extensions"), duckdb::Value(false));
        config.SetOptionByName(duckdb::Identifier("allow_persistent_secrets"), duckdb::Value(false));
        config.SetOptionByName(duckdb::Identifier("threads"), duckdb::Value::BIGINT(1));
        config.SetOptionByName(duckdb::Identifier("memory_limit"), duckdb::Value("64MiB"));
        duckdb::DuckDB candidate(path.c_str(), &config);
        AuditNativeStoredDefinitions(candidate);
        CheckIdentity(path, expected);
    } catch (...) {
        // Engine catalog-loading errors can include stored expression text.
        throw std::runtime_error("Read-copy version validation failed");
    }
}
}

struct ReadCopyController::Impl {
    fs::path root, pointer, storage_directory, active_path;
    PathCheck path_check;
    AfterPublish after_publish;
    duckdb::Connection controller;
    struct stat active_identity {};
    std::string attempted;
    std::unordered_map<std::string, std::string> version_identities;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> available = std::make_shared<std::atomic<bool>>(true);

    Impl(duckdb::DuckDB &host, fs::path root_p, fs::path pointer_p, PathCheck check, AfterPublish hook)
        : root(fs::canonical(root_p)), pointer(PointerPath(root, pointer_p)), path_check(std::move(check)),
          after_publish(std::move(hook)), controller(host) {
        storage_directory = ReadCopyController::VersionDirectory(root, pointer);
        const bool acknowledged = HasAck(pointer);
        const auto candidate = ReadPointer(root, pointer, acknowledged ? fs::path(pointer.string() + ".active") : fs::path());
        if (path_check) path_check(candidate);
        File file(candidate, false);
        ValidateVersion(candidate, file.status);
        Attach(candidate);
        Query(controller, "USE base");
        Query(controller, "DETACH memory");
        CheckCatalog(candidate);
        if (!acknowledged) SaveAck(root, pointer, candidate);
        active_path = candidate; active_identity = file.status; generation = 1;
        version_identities.emplace(candidate.string(), Fingerprint(file.status));
        attempted = acknowledged ? std::string() : PointerFingerprint(pointer);
    }

    void Attach(const fs::path &path) {
        // Publication must always be autocommit. Rolling an ATTACH replacement
        // back can remove the alias entirely in the paired native engine.
        if (!controller.IsAutoCommit()) throw std::runtime_error("Read-copy controller transaction is not idle");
        Query(controller, "ATTACH OR REPLACE " + Quote(path.string()) + " AS base (TYPE DUCKDB, READ_ONLY)");
    }

    void CheckCatalog(const fs::path &path) {
        auto result = controller.Query("SELECT path, readonly FROM system.main.duckdb_databases() WHERE database_name='base'");
        if (!result || result->HasError() || result->RowCount() != 1 ||
            result->GetValue(0, 0).GetValue<std::string>() != path.string() || !result->GetValue<bool>(1, 0))
            throw std::runtime_error("Read-copy catalog verification failed");
    }

    Result Poll() {
        bool invalidated = false;
        if (available->load()) {
            try {
                LocalPath(root, active_path);
                ReadCopyController::VersionDirectory(root, pointer);
                CheckIdentity(active_path, active_identity);
            } catch (...) {
                available->store(false);
                invalidated = true;
            }
        }
        const auto fingerprint = PointerFingerprint(pointer);
        if (fingerprint == attempted) return invalidated ? Result::Unavailable : Result::Unchanged;
        attempted = fingerprint;
        try {
            const auto candidate = ReadPointer(root, pointer);
            if (path_check) path_check(candidate);
            File file(candidate, false);
            auto seen = version_identities.find(candidate.string());
            if (seen != version_identities.end() && seen->second != Fingerprint(file.status)) Invalid();
            if (seen == version_identities.end() && version_identities.size() >= 4096)
                return invalidated ? Result::Unavailable : Result::VersionLimit;
            if (candidate == active_path && available->load()) {
                CheckIdentity(candidate, active_identity);
                return Result::Unchanged;
            }
            ValidateVersion(candidate, file.status);
            version_identities.emplace(candidate.string(), Fingerprint(file.status));
            try {
                Attach(candidate);
                if (after_publish) after_publish(controller);
                CheckIdentity(candidate, file.status);
                CheckCatalog(candidate);
                SaveAck(root, pointer, candidate);
                active_path = candidate; active_identity = file.status;
                ++generation; available->store(true);
                return Result::Published;
            } catch (...) {
                try {
                    if (!controller.IsAutoCommit()) controller.Rollback();
                    CheckIdentity(active_path, active_identity);
                    ValidateVersion(active_path, active_identity);
                    Attach(active_path);
                    CheckCatalog(active_path);
                    const auto acknowledged = ReadPointer(root, pointer, pointer.string() + ".active");
                    if (acknowledged != active_path) SaveAck(root, pointer, active_path);
                    available->store(true);
                    return Result::Restored;
                } catch (...) {
                    available->store(false);
                    return Result::Unavailable;
                }
            }
        } catch (...) {
            return invalidated ? Result::Unavailable : Result::Rejected;
        }
    }
};

ReadCopyController::ReadCopyController(duckdb::DuckDB &host, fs::path root, fs::path pointer,
                                     PathCheck check, AfterPublish hook)
    : impl(std::make_unique<Impl>(host, std::move(root), std::move(pointer), std::move(check), std::move(hook))) {}
ReadCopyController::~ReadCopyController() = default;
ReadCopyController::Result ReadCopyController::Poll() { return impl->Poll(); }
const fs::path &ReadCopyController::Path() const { return impl->active_path; }
const fs::path &ReadCopyController::Pointer() const { return impl->pointer; }
const fs::path &ReadCopyController::StorageDirectory() const { return impl->storage_directory; }
std::uint64_t ReadCopyController::Generation() const { return impl->generation; }
std::shared_ptr<std::atomic<bool>> ReadCopyController::Available() const { return impl->available; }
fs::path ReadCopyController::InitialPath(const fs::path &root, const fs::path &pointer) {
    const auto path = ReadPointer(fs::canonical(root), pointer,
        HasAck(pointer.is_absolute() ? pointer : root / pointer) ? fs::path((pointer.is_absolute() ? pointer : root / pointer).string() + ".active") : fs::path());
    File file(path, false);
    CheckIdentity(path, file.status);
    return path;
}
fs::path ReadCopyController::VersionDirectory(const fs::path &root, const fs::path &pointer) {
    const auto canonical_root = fs::canonical(root);
    const auto canonical_pointer = PointerPath(canonical_root, pointer);
    const auto directory = LocalPath(canonical_root, fs::path(canonical_pointer.string() + ".versions"));
    struct stat status {};
    if (lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 07777) != 0700) Invalid();
    return directory;
}
}
