#include "duckdb.hpp"
#include "duckonwire/pgwire.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Reject(duckdb::DuckDB &primary, std::vector<duckonwire::PgDatabase> routes) {
    duckonwire::PgWireServer server(primary, "127.0.0.1", 0, "isolated-routing-fixture", 1, 1000, std::move(routes));
    try { server.Start(); }
    catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Invalid database route was accepted");
}
}

int main() {
    try {
        duckdb::DuckDB first(nullptr), second(nullptr);
        duckdb::DuckDB wrapper(*first.instance);
        // Different wrapper addresses still represent one shared native engine.
        Reject(first, {{"first", &first}, {"second", &wrapper}});
        Reject(first, {{"first", &first}, {"FIRST", &second}});
        Reject(first, {{"first", &first}, {"other", nullptr}});
        Reject(first, {{"first", &first}, {"temp", &second}});
        Reject(first, {{"second", &second}});
        duckonwire::PgWireServer valid(first, "127.0.0.1", 0, "isolated-routing-fixture", 1, 1000,
                                      {{"first", &first}, {"second", &second}});
        valid.Start();
        if (!valid.Port()) throw std::runtime_error("Valid isolated routes did not start");
        valid.Stop();
        std::cout << "{\"passed\":true,\"checks\":["
                     "{\"name\":\"shared_native_instance_rejected\",\"passed\":true},"
                     "{\"name\":\"case_folded_duplicate_rejected\",\"passed\":true},"
                     "{\"name\":\"null_route_rejected\",\"passed\":true},"
                     "{\"name\":\"reserved_catalog_rejected\",\"passed\":true},"
                     "{\"name\":\"missing_primary_rejected\",\"passed\":true},"
                     "{\"name\":\"independent_native_instances_start\",\"passed\":true}]}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Database routing checks failed: " << error.what() << '\n';
        return 1;
    }
}
