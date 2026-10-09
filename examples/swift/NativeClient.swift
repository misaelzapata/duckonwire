import Foundation

enum ProbeError: Error { case failed(String) }
var checks: [String] = []

func require(_ condition: Bool, _ stage: String) throws {
    if !condition { throw ProbeError.failed(stage) }
}

func literal(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "''") + "'" }

func query(_ connection: duckdb_connection?, _ sql: String) throws -> duckdb_result {
    try require(connection != nil, "connection_open")
    var result = duckdb_result()
    let status = sql.withCString { duckdb_query(connection, $0, &result) }
    if status != DuckDBSuccess {
        // SQL bootstrap can contain the bearer token; diagnostics use the stage only.
        duckdb_destroy_result(&result)
        throw ProbeError.failed("native_query")
    }
    return result
}

func execute(_ connection: duckdb_connection?, _ sql: String) throws {
    var result = try query(connection, sql)
    duckdb_destroy_result(&result)
}

func number(_ connection: duckdb_connection?, _ sql: String) throws -> Int64 {
    var result = try query(connection, sql)
    defer { duckdb_destroy_result(&result) }
    try require(duckdb_row_count(&result) == 1, "scalar_row_count")
    return duckdb_value_int64(&result, 0, 0)
}

func main() throws -> [String: Any] {
    let root = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL.resolvingSymlinksInPath()
    let state = URL(fileURLWithPath: CommandLine.arguments[2]).standardizedFileURL.resolvingSymlinksInPath()
    try require(state.path.hasPrefix(root.path + "/"), "descriptor_inside_root")
    let descriptor = try JSONSerialization.jsonObject(with: Data(contentsOf: state)) as! [String: Any]
    let endpoint = descriptor["endpoint"] as! String
    let token = descriptor["token"] as! String
    var db: duckdb_database?
    var writer: duckdb_connection?
    var reader: duckdb_connection?
    try require(duckdb_open(nil, &db) == DuckDBSuccess, "native_open")
    defer { duckdb_close(&db) }
    try require(duckdb_connect(db, &writer) == DuckDBSuccess && duckdb_connect(db, &reader) == DuckDBSuccess, "native_connections")
    defer { duckdb_disconnect(&reader); duckdb_disconnect(&writer) }
    try require(try number(writer, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded") == 2,
                "builtin_httpfs_quack_without_LOAD")
    checks.append("builtin_httpfs_quack_without_LOAD")
    for connection in [writer, reader] {
        try execute(connection, "CONNECT " + literal(endpoint) + " (TOKEN " + literal(token) + ")")
    }
    checks.append("direct_native_C_interop_two_connections")
    let table = "codex_swift_native_\(ProcessInfo.processInfo.processIdentifier)"
    try execute(writer, "CREATE TABLE \(table)(id INTEGER PRIMARY KEY,value BIGINT,label VARCHAR,payload BLOB)")
    defer { duckdb_disconnect(&writer); try? execute(reader, "DROP TABLE IF EXISTS \(table)") }
    var prepared: duckdb_prepared_statement?
    try require("INSERT INTO \(table) VALUES (?,?,?,?)".withCString { duckdb_prepare(writer, $0, &prepared) } == DuckDBSuccess, "native_prepare")
    defer { duckdb_destroy_prepare(&prepared) }
    try require(duckdb_bind_int32(prepared, 1, 1) == DuckDBSuccess, "bind_integer")
    try require(duckdb_bind_int64(prepared, 2, 9007199254740993) == DuckDBSuccess, "bind_int64")
    try require("Swift ñ 🍊".withCString { duckdb_bind_varchar(prepared, 3, $0) } == DuckDBSuccess, "bind_unicode")
    let bytes: [UInt8] = [0, 1, 255]
    try require(bytes.withUnsafeBytes { duckdb_bind_blob(prepared, 4, $0.baseAddress, UInt64($0.count)) } == DuckDBSuccess, "bind_binary")
    var inserted = duckdb_result()
    try require(duckdb_execute_prepared(prepared, &inserted) == DuckDBSuccess, "native_execute_prepared")
    duckdb_destroy_result(&inserted)
    var typed = try query(reader, "SELECT value,label,payload FROM \(table) WHERE id=1")
    let label = duckdb_value_varchar(&typed, 1, 0)
    let blob = duckdb_value_blob(&typed, 2, 0)
    defer { duckdb_free(label); duckdb_free(blob.data); duckdb_destroy_result(&typed) }
    try require(duckdb_value_int64(&typed, 0, 0) == 9007199254740993, "exact_int64")
    try require(label.map { String(cString: $0) } == "Swift ñ 🍊", "unicode_roundtrip")
    try require(blob.size == 3 && blob.data != nil && Array(UnsafeBufferPointer(start: blob.data!.assumingMemoryBound(to: UInt8.self), count: Int(blob.size))) == bytes, "binary_roundtrip")
    checks.append("native_prepared_int64_unicode_blob_roundtrip")
    try execute(writer, "BEGIN")
    try execute(writer, "UPDATE \(table) SET value=42 WHERE id=1")
    let started = Date()
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 9007199254740993, "reader_snapshot")
    try require(Date().timeIntervalSince(started) < 2, "reader_nonblocking")
    try execute(writer, "ROLLBACK")
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 9007199254740993, "rollback")
    try execute(writer, "BEGIN")
    try execute(writer, "UPDATE \(table) SET value=43 WHERE id=1")
    try execute(writer, "COMMIT")
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 43, "commit")
    checks.append("native_mvcc_nonblocking_commit_rollback")
    try execute(reader, "BEGIN")
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 43, "reader_snapshot_start")
    try execute(writer, "UPDATE \(table) SET value=44 WHERE id=1")
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 43, "reader_snapshot_after_writer_commit")
    try execute(reader, "COMMIT")
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 44, "reader_new_snapshot")
    try execute(writer, "UPDATE \(table) SET value=43 WHERE id=1")
    checks.append("reader_transaction_snapshot_isolation")
    if try number(writer, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'") > 0 {
        var shared = try query(reader, "SELECT id,label FROM lang_multiclient_probe ORDER BY id")
        for row in 0..<duckdb_row_count(&shared) {
            try require(!duckdb_value_is_null(&shared, 1, row), "read_other_language_markers")
        }
        duckdb_destroy_result(&shared)
        var marker: duckdb_prepared_statement?
        try require("INSERT INTO lang_multiclient_probe VALUES(?,?)".withCString { duckdb_prepare(writer, $0, &marker) } == DuckDBSuccess, "prepare_interop_marker")
        defer { duckdb_destroy_prepare(&marker) }
        try require(duckdb_bind_int32(marker, 1, 7) == DuckDBSuccess && "from Swift".withCString { duckdb_bind_varchar(marker, 2, $0) } == DuckDBSuccess, "bind_interop_marker")
        var result = duckdb_result()
        try require(duckdb_execute_prepared(marker, &result) == DuckDBSuccess, "execute_interop_marker")
        duckdb_destroy_result(&result)
        try require(try number(reader, "SELECT count(*) FROM lang_multiclient_probe WHERE id=7 AND label='from Swift'") == 1, "independent_reader_interop_marker")
        checks.append("shared_multilanguage_table_read_and_write")
    }
    try execute(writer, "BEGIN")
    try execute(writer, "UPDATE \(table) SET value=99 WHERE id=1")
    duckdb_disconnect(&writer)
    try require(try number(reader, "SELECT value FROM \(table) WHERE id=1") == 43, "close_rollback")
    try execute(reader, "DROP TABLE \(table)")
    checks.append("native_close_rollback")
    return ["passed": true, "native_used": true, "language": "Swift", "driver": "native C API through Swift compiler interop", "checks": checks,
            "version": String(cString: duckdb_library_version())]
}

let started = Date()
var report: [String: Any]
do { report = try main() }
catch { report = ["passed": false, "native_used": true, "checks": checks, "error": String(describing: error)] }
report["seconds"] = Date().timeIntervalSince(started)
let output = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
print(String(decoding: output, as: UTF8.self))
if report["passed"] as? Bool != true { exit(1) }
