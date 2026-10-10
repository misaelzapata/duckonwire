#pragma once

#include <cstdint>
#include <filesystem>

namespace duckdb {
class Connection;
}

namespace duckonwire {
namespace demo {

void Initialize(duckdb::Connection &owner, const std::filesystem::path &root, std::int64_t price_rows = 0);

} // namespace demo
} // namespace duckonwire
