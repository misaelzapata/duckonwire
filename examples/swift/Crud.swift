import Foundation

enum Failure: Error { case stage(String) }
enum Parameter { case text(String), integer(Int32), cents(UInt64) }

func require(_ success: Bool, _ stage: String) throws {
    if !success { throw Failure.stage(stage) }
}

func run(_ connection: duckdb_connection?, _ sql: String, _ parameters: [Parameter] = []) throws -> duckdb_result {
    if parameters.isEmpty {
        var result = duckdb_result()
        if sql.withCString({ duckdb_query(connection, $0, &result) }) != DuckDBSuccess {
            duckdb_destroy_result(&result)
            throw Failure.stage("query")
        }
        return result
    }
    var statement: duckdb_prepared_statement?
    try require(sql.withCString { duckdb_prepare(connection, $0, &statement) } == DuckDBSuccess, "prepare")
    defer { duckdb_destroy_prepare(&statement) }
    for (offset, parameter) in parameters.enumerated() {
        let index = UInt64(offset + 1)
        let status: duckdb_state
        switch parameter {
        case .text(let value): status = value.withCString { duckdb_bind_varchar(statement, index, $0) }
        case .integer(let value): status = duckdb_bind_int32(statement, index, value)
        case .cents(let value):
            status = duckdb_bind_decimal(statement, index,
                duckdb_decimal(width: 18, scale: 2, value: duckdb_hugeint(lower: value, upper: 0)))
        }
        try require(status == DuckDBSuccess, "bind")
    }
    var result = duckdb_result()
    if duckdb_execute_prepared(statement, &result) != DuckDBSuccess {
        duckdb_destroy_result(&result)
        throw Failure.stage("execute")
    }
    return result
}

func change(_ connection: duckdb_connection?, _ sql: String, _ parameters: [Parameter] = []) throws {
    var result = try run(connection, sql, parameters)
    duckdb_destroy_result(&result)
}

func rows(_ connection: duckdb_connection?) throws -> [[String: Any]] {
    var result = try run(connection, "SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id", [.text("Swift")])
    defer { duckdb_destroy_result(&result) }
    func string(_ column: UInt64, _ row: UInt64) -> String {
        let value = duckdb_value_varchar(&result, column, row)
        defer { duckdb_free(value) }
        return value.map { String(cString: $0) } ?? ""
    }
    return (0..<duckdb_row_count(&result)).map { row in
        ["id": Int(duckdb_value_int32(&result, 0, row)), "name": string(1, row),
         "price": string(2, row), "stock": Int(duckdb_value_int32(&result, 3, row))]
    }
}

func main() throws -> [String: Any] {
    let environment = ProcessInfo.processInfo.environment
    let endpoint = environment["DUCKONWIRE_ENDPOINT"] ?? "quack:127.0.0.1:9495"
    let token = environment["DUCKONWIRE_TOKEN"] ?? ""
    try require(!token.isEmpty, "DUCKONWIRE_TOKEN must be set")
    func literal(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "''") + "'" }
    var database: duckdb_database?
    var connection: duckdb_connection?
    try require(duckdb_open(nil, &database) == DuckDBSuccess, "open")
    defer { duckdb_close(&database) }
    try require(duckdb_connect(database, &connection) == DuckDBSuccess, "connect")
    defer { duckdb_disconnect(&connection) }
    try change(connection, "CONNECT " + literal(endpoint) + " (TOKEN " + literal(token) + ")")
    try change(connection, "CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))")
    try change(connection, "BEGIN")
    try change(connection, "DELETE FROM demo_products WHERE client=?", [.text("Swift")])
    let products: [(Int32, String, UInt64, Int32)] = [(1, "Cafe", 1999, 3), (2, "Arroz", 850, 10)]
    for (id, name, price, stock) in products {
        try change(connection, "INSERT INTO demo_products VALUES(?,?,?,?,?)", [.text("Swift"), .integer(id), .text(name), .cents(price), .integer(stock)])
    }
    try change(connection, "UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?", [.text("Swift"), .integer(1)])
    let before = try rows(connection)
    try change(connection, "DELETE FROM demo_products WHERE client=? AND id=?", [.text("Swift"), .integer(2)])
    try change(connection, "COMMIT")
    let after = try rows(connection)
    try require(after.count == 1 && after[0]["id"] as? Int == 1 && after[0]["name"] as? String == "Cafe"
        && after[0]["price"] as? String == "19.99" && after[0]["stock"] as? Int == 4, "persisted_product")
    func arrays(_ products: [[String: Any]]) -> [[Any]] {
        products.map { [$0["id"]!, $0["name"]!, $0["price"]!, $0["stock"]!] }
    }
    return ["passed": true, "client": "Swift", "before_delete": arrays(before), "rows": arrays(after)]
}

let report: [String: Any]
do { report = try main() }
catch { report = ["passed": false, "client": "Swift", "error": String(describing: error)] }
let encoded = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
print(String(decoding: encoded, as: UTF8.self))
if report["passed"] as? Bool != true { exit(1) }
