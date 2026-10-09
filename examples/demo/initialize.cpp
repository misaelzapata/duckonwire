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

void Initialize(duckdb::Connection &owner, const fs::path &root) {
	auto tables = owner.Query("SELECT count(*) FROM duckdb_tables() WHERE NOT internal AND NOT temporary");
	if (tables->HasError()) tables->ThrowError();
	if (tables->GetValue<int64_t>(0, 0) != 0) {
		throw std::runtime_error("--init-demo requires an empty database; existing tables are preserved");
	}
	const auto products = OwnedPath("examples/demo/products.csv", root);
	const auto prices = OwnedPath("examples/demo/prices.csv", root);
	if (!fs::is_regular_file(products) || !fs::is_regular_file(prices)) {
		throw std::runtime_error("Demo CSV files are missing under examples/demo");
	}
	owner.BeginTransaction();
	try {
		auto schema = owner.Query("CREATE TABLE products(id INTEGER PRIMARY KEY,name VARCHAR,brand VARCHAR,category VARCHAR,size VARCHAR,unit VARCHAR,country_code VARCHAR); CREATE TABLE prices(id INTEGER PRIMARY KEY,product_id INTEGER,price DECIMAL(18,2),day DATE)");
		if (schema->HasError()) schema->ThrowError();
		auto a = owner.Query("INSERT INTO products SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', columns={'id':'INTEGER','name':'VARCHAR','brand':'VARCHAR','category':'VARCHAR','size':'VARCHAR','unit':'VARCHAR','country_code':'VARCHAR'})", duckdb::Value(products.string()));
		if (a->HasError()) a->ThrowError();
		auto b = owner.Query("INSERT INTO prices SELECT * FROM read_csv(?, header=true, quote='\"', escape='\"', columns={'id':'INTEGER','product_id':'INTEGER','price':'DECIMAL(18,2)','day':'DATE'})", duckdb::Value(prices.string()));
		if (b->HasError()) b->ThrowError();
		auto valid = owner.Query("SELECT (SELECT count(*) FROM products)=46462 AND (SELECT count(*) FROM prices)=46462 AND NOT EXISTS(SELECT 1 FROM products WHERE country_code NOT IN ('AR','ES')) AND NOT EXISTS(SELECT 1 FROM prices p LEFT JOIN products x ON p.product_id=x.id WHERE x.id IS NULL)");
		if (valid->HasError()) valid->ThrowError();
		if (!valid->GetValue<bool>(0, 0)) throw std::runtime_error("Demo CSV integrity check failed");
		owner.Commit();
	} catch (...) { owner.Rollback(); throw; }
}

} // namespace demo
} // namespace duckonwire
