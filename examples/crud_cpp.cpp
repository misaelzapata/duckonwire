#include "duckdb.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace duckdb;
template<class T> static void Check(const unique_ptr<T> &result) {
    if (result->HasError()) result->ThrowError();
}
static string Quote(const string &value) {
    string out = "'";
    for (char ch : value) out += ch == '\'' ? "''" : string(1, ch);
    return out + "'";
}
int main() {
    try {
        const char *endpoint_env = std::getenv("DUCKONWIRE_ENDPOINT");
        const char *token_env = std::getenv("DUCKONWIRE_TOKEN");
        if (!token_env) throw std::runtime_error("Set DUCKONWIRE_TOKEN");
        string endpoint = endpoint_env ? endpoint_env : "quack:127.0.0.1:9495";
        string token = token_env;
        DuckDB database(nullptr);
        Connection con(database);
        Check(con.Query("CONNECT " + Quote(endpoint) + " (TOKEN " + Quote(token) + ")"));
        Check(con.Query("CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))"));
        con.BeginTransaction();
        Check(con.Query("DELETE FROM demo_products WHERE client=?", Value("C++")));
        Check(con.Query("INSERT INTO demo_products VALUES (?,?,?,?,?)", Value("C++"), Value(1), Value("Cafe"), Value::DECIMAL(int64_t(1999),18,2), Value(3)));
        Check(con.Query("INSERT INTO demo_products VALUES (?,?,?,?,?)", Value("C++"), Value(2), Value("Arroz"), Value::DECIMAL(int64_t(850),18,2), Value(10)));
        Check(con.Query("UPDATE demo_products SET stock=stock+? WHERE client=? AND id=?", Value(1), Value("C++"), Value(1)));
        auto before = con.Query("SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id", Value("C++"));
        Check(before);
        auto before_rows = before->Fetch();
        if (!before_rows || before_rows->size() != 2 || before_rows->GetValue(3,0).GetValue<int32_t>() != 4) throw std::runtime_error("updated rows differ");
        Check(con.Query("DELETE FROM demo_products WHERE client=? AND id=?", Value("C++"), Value(2)));
        con.Commit();
        auto rows = con.Query("SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id", Value("C++"));
        Check(rows);
        auto final_rows = rows->Fetch();
        if (!final_rows || final_rows->size() != 1 || final_rows->GetValue(2,0).ToString() != "19.99" || final_rows->GetValue(3,0).GetValue<int32_t>() != 4) throw std::runtime_error("committed rows differ");
        auto output = con.Query("SELECT json_array(id,name,CAST(price AS VARCHAR),stock) FROM demo_products WHERE client=? ORDER BY id", Value("C++"));
        Check(output);
        auto output_rows = output->Fetch();
        std::cout << "{\"passed\":true,\"language\":\"C++\",\"rows\":[" << output_rows->GetValue(0,0).ToString() << "]}" << std::endl;
        return 0;
    } catch (...) {
        std::cerr << "Native CRUD failed; pending changes were rolled back on close" << std::endl;
        return 1;
    }
}
