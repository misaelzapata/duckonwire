#include "duckdb.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {
template <class T> void Check(const duckdb::unique_ptr<T> &result) {
    if (!result || result->HasError()) throw std::runtime_error("Cannot create example tables");
}

void Sync(const fs::path &path, bool directory = false) {
    const auto flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0);
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) throw std::runtime_error("Cannot open prepared example data");
    const int synced = fsync(descriptor);
    const int closed = close(descriptor);
    if (synced != 0 || closed != 0) throw std::runtime_error("Cannot persist example data");
}

void PrepareCopy(const fs::path &path, int version) {
    {
        duckdb::DBConfig config;
        config.SetOptionByName("threads", duckdb::Value::BIGINT(2));
        duckdb::DuckDB database(path.c_str(), &config);
        duckdb::Connection connection(database);
        connection.BeginTransaction();
        Check(connection.Query("CREATE TABLE inventory(id INTEGER PRIMARY KEY, name VARCHAR, stock INTEGER)"));
        Check(connection.Query("INSERT INTO inventory VALUES (1, 'Coffee', ?)", duckdb::Value::INTEGER(version + 2)));
        Check(connection.Query("CREATE TABLE copy_version(version INTEGER PRIMARY KEY)"));
        Check(connection.Query("INSERT INTO copy_version VALUES (?)", duckdb::Value::INTEGER(version)));
        connection.Commit();
        Check(connection.Query("CHECKPOINT"));
    } // Close the database before marking its completed file immutable.
    if (chmod(path.c_str(), 0400) != 0) throw std::runtime_error("Cannot protect example copy");
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 || (status.st_mode & 07777) != 0400)
        throw std::runtime_error("Invalid example copy permissions");
    Sync(path);
}

std::string ShellQuote(const std::string &value) {
    std::string result = "'";
    for (const char character : value) result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 || std::string(argv[1]).empty()) {
        std::cerr << "Usage: read-copy-example NEW_WORKSPACE\n";
        return 2;
    }
    try {
        const auto root = fs::absolute(argv[1]).lexically_normal();
        umask(0077);
        // mkdir fails for an existing directory, file or symlink; never reuse it.
        if (mkdir(root.c_str(), 0700) != 0)
            throw std::runtime_error("Choose a new workspace path with an existing parent directory");
        const auto copies = root / "copies";
        const auto pointer = copies / "catalog.current";
        const auto versions = copies / "catalog.current.versions";
        if (mkdir(copies.c_str(), 0700) != 0 || mkdir(versions.c_str(), 0700) != 0)
            throw std::runtime_error("Cannot create private example directories");
        PrepareCopy(versions / "v1.duckdb", 1);
        PrepareCopy(versions / "v2.duckdb", 2);
        Sync(versions, true);
        {
            std::ofstream publication(pointer, std::ios::binary);
            publication << "v1.duckdb\n";
            publication.close();
            if (!publication) throw std::runtime_error("Cannot prepare example pointer");
        }
        if (chmod(pointer.c_str(), 0600) != 0) throw std::runtime_error("Cannot protect example pointer");
        Sync(pointer);
        Sync(copies, true);
        Sync(root, true);
        Sync(root.parent_path(), true);
        std::cout << "Prepared two example copies: Coffee stock 3 (version 1), then 4 (version 2).\n"
                  << "Start: duckonwire-server --root " << ShellQuote(root.string())
                  << " --read-copy shop=copies/catalog.current\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Example preparation failed: " << error.what() << '\n';
        return 1;
    }
}
