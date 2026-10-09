#pragma once

#include <filesystem>

namespace duckdb {
class Connection;
}

namespace duckonwire {
namespace demo {

void Initialize(duckdb::Connection &owner, const std::filesystem::path &root);

} // namespace demo
} // namespace duckonwire
