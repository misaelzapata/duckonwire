import Foundation

func env(_ name: String, _ fallback: String) -> String { ProcessInfo.processInfo.environment[name] ?? fallback }
let sslMode=env("PGSSLMODE","disable")
guard sslMode=="disable" || sslMode=="verify-full" else {fputs("Use PGSSLMODE=disable or verify-full\n",stderr);exit(2)}
guard sslMode != "verify-full" || !env("PGSSLROOTCERT","").isEmpty else {fputs("Set PGSSLROOTCERT for verify-full\n",stderr);exit(2)}
let settings = [env("PGHOST", "127.0.0.1"), env("PGPORT", "9496"), env("PGUSER", "duckonwire"), env("PGDATABASE", "duckonwire"), env("PGPASSWORD", env("DUCKONWIRE_TOKEN", "")), sslMode, "5"]
let keywords: [String] = ["host", "port", "user", "dbname", "password", "sslmode", "connect_timeout"]
let keyStrings = keywords.map { strdup($0) }
let valueStrings = settings.map { strdup($0) }
defer { keyStrings.forEach { free($0) }; valueStrings.forEach { free($0) } }
var keys = keyStrings.map { $0.map { UnsafePointer<CChar>($0) } } + [nil]
var values = valueStrings.map { $0.map { UnsafePointer<CChar>($0) } } + [nil]
let connection = PQconnectdbParams(&keys, &values, 0)!
defer { PQfinish(connection) }
enum Failure: Error { case database(String) }
func query(_ sql: String, _ parameters: [String]? = nil) throws -> OpaquePointer {
    let result: OpaquePointer
    if let parameters {
        let copied = parameters.map { strdup($0) }
        defer { copied.forEach { free($0) } }
        var bound = copied.map { $0.map { UnsafePointer<CChar>($0) } }
        result = PQexecParams(connection, sql, Int32(bound.count), nil, &bound, nil, nil, 0)!
    } else { result = PQexec(connection, sql)! }
    let status = PQresultStatus(result)
    if status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK {
        let message = String(cString: PQresultErrorMessage(result)); PQclear(result); throw Failure.database(message)
    }
    return result
}
func command(_ sql: String, _ parameters: [String]? = nil) throws { PQclear(try query(sql, parameters)) }
do {
    guard PQstatus(connection) == CONNECTION_OK else { throw Failure.database(String(cString: PQerrorMessage(connection))) }
    let owner = "PostgreSQL Swift"
    try command("CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))")
    try command("BEGIN")
    try command("DELETE FROM pg_demo_products WHERE client=$1", [owner])
    try command("INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)", [owner,"1","Cafe","19.99","3"])
    try command("INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)", [owner,"2","Arroz","8.50","10"])
    try command("UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2", [owner,"1"])
    try command("DELETE FROM pg_demo_products WHERE client=$1 AND id=$2", [owner,"2"])
    try command("COMMIT")
    let result = try query("SELECT id,name,price,stock FROM pg_demo_products WHERE client=$1 ORDER BY id", [owner])
    defer { PQclear(result) }
    guard PQntuples(result) == 1 else { throw Failure.database("Unexpected row count") }
    let row: [Any] = [Int(String(cString:PQgetvalue(result,0,0)))!, String(cString:PQgetvalue(result,0,1)), String(cString:PQgetvalue(result,0,2)), Int(String(cString:PQgetvalue(result,0,3)))!]
    guard row[0] as! Int == 1, row[1] as! String == "Cafe", row[2] as! String == "19.99", row[3] as! Int == 4 else { throw Failure.database("Unexpected committed row") }
    let output: [String: Any] = ["passed": true, "client": owner, "driver": "libpq", "rows": [row]]
    print(String(data:try JSONSerialization.data(withJSONObject: output, options: [.sortedKeys]), encoding:.utf8)!)
} catch { fputs("\(error)\n", stderr); exit(1) }
