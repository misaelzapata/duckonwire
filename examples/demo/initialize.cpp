#include "initialize.hpp"
#include "duckdb.hpp"

#include <cstdint>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
bool Within(const fs::path &path, const fs::path &root) {
	auto part = path.begin();
	for (auto base = root.begin(); base != root.end(); ++base, ++part) {
		if (part == path.end() || *part != *base) return false;
	}
	return part != path.end();
}

fs::path OwnedPath(const fs::path &input, const fs::path &root) {
	auto path = fs::weakly_canonical(input.is_absolute() ? input : root / input);
	if (!Within(path, root)) throw std::runtime_error("Paths must remain inside --root");
	return path;
}
} // namespace

namespace duckonwire {
namespace demo {

void Initialize(duckdb::Connection &owner, const fs::path &root, std::int64_t price_rows) {
	if (price_rows != 0 && (price_rows < 46462 || price_rows > 10000000)) {
		throw std::runtime_error("Demo price rows must be between 46462 and 10000000");
	}
	auto tables = owner.Query("SELECT count(*) FROM duckdb_tables() WHERE NOT internal AND NOT temporary");
	if (tables->HasError()) tables->ThrowError();
	if (tables->GetValue<int64_t>(0, 0) != 0) {
		throw std::runtime_error("--init-demo requires an empty database; existing tables are preserved");
	}
	const auto products = OwnedPath("examples/demo/products.csv", root);
	const auto prices = OwnedPath("examples/demo/prices.csv", root);
	if (!fs::is_regular_file(products) || (price_rows == 0 && !fs::is_regular_file(prices))) {
		throw std::runtime_error("Demo CSV files are missing under examples/demo");
	}
	owner.BeginTransaction();
	try {
		auto schema = owner.Query(R"SQL(
CREATE TABLE products (
    id INTEGER PRIMARY KEY,
    name VARCHAR NOT NULL CHECK (length(name)>0),
    brand VARCHAR NOT NULL CHECK (length(brand)>0),
    category VARCHAR NOT NULL,
    size VARCHAR NOT NULL,
    unit VARCHAR NOT NULL,
    country_code VARCHAR NOT NULL CHECK (country_code IN ('AR','ES'))
);
CREATE TABLE prices (
    id INTEGER PRIMARY KEY,
    product_id INTEGER NOT NULL REFERENCES products(id),
    price DECIMAL(18,2) NOT NULL CHECK (price BETWEEN 0.50 AND 250.00),
    day DATE NOT NULL
))SQL");
		if (schema->HasError()) schema->ThrowError();
		auto a = owner.Query("INSERT INTO products SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', force_not_null=['category','size','unit'], columns={'id':'INTEGER','name':'VARCHAR','brand':'VARCHAR','category':'VARCHAR','size':'VARCHAR','unit':'VARCHAR','country_code':'VARCHAR'})", duckdb::Value(products.string()));
		if (a->HasError()) a->ThrowError();
		auto b = price_rows == 0
		    ? owner.Query("INSERT INTO prices SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', columns={'id':'INTEGER','product_id':'INTEGER','price':'DECIMAL(18,2)','day':'DATE'})", duckdb::Value(prices.string()))
		    : owner.Query("INSERT INTO prices SELECT i::INTEGER, (1 + (i - 1) % 46462)::INTEGER, "
		                  "((50 + (i * 48271 + 12345) % 24951) * 0.01)::DECIMAL(18,2), "
		                  "DATE '2026-01-01' + (((i * 1103515245 + 12345) % 2147483648) % 30)::INTEGER "
		                  "FROM range(1, ?::BIGINT + 1) AS generated(i)", duckdb::Value::BIGINT(price_rows));
		if (b->HasError()) b->ThrowError();
		const auto expected_rows = price_rows == 0 ? 46462 : price_rows;
		auto valid = owner.Query("SELECT (SELECT count(*) FROM products)=46462 AND (SELECT count(*) FROM prices)=? "
		                         "AND NOT EXISTS(SELECT 1 FROM products WHERE country_code NOT IN ('AR','ES')) "
		                         "AND NOT EXISTS(SELECT 1 FROM prices p LEFT JOIN products x ON p.product_id=x.id WHERE x.id IS NULL)",
		                         duckdb::Value::BIGINT(expected_rows));
		if (valid->HasError()) valid->ThrowError();
		auto valid_row = valid->Fetch();
		if (!valid_row || valid_row->size() != 1 || !valid_row->GetValue(0, 0).GetValue<bool>())
			throw std::runtime_error("Demo CSV integrity check failed");
		if (price_rows != 0) {
			auto history = owner.Query("SELECT min(id)=1 AND max(id)=? AND sum(id::BIGINT)=? "
			                           "AND min(price)>=0.50 AND max(price)<=250.00 "
			                           "AND min(day)=DATE '2026-01-01' AND max(day)=DATE '2026-01-30' "
			                           "AND count(DISTINCT day)=30 FROM prices",
			                           duckdb::Value::BIGINT(price_rows),
			                           duckdb::Value::BIGINT(price_rows * (price_rows + 1) / 2));
			if (history->HasError()) history->ThrowError();
			auto history_row = history->Fetch();
			if (!history_row || history_row->size() != 1 || !history_row->GetValue(0, 0).GetValue<bool>())
				throw std::runtime_error("Synthetic price history integrity check failed");
		}
		owner.Commit();
	} catch (...) { owner.Rollback(); throw; }
}

} // namespace demo
} // namespace duckonwire
