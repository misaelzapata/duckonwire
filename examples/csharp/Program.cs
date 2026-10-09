using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using DuckDB.NET.Data;
using DuckDB.NET.Native;

// Managed-only, unmodified ADO.NET provider; Microsoft's resolver loads the paired native engine.
var checks = new List<string>();
string token = "", version = "", error = "";
bool passed = false;
var stopwatch = Stopwatch.StartNew();
string table = $"codex_csharp_native_{Environment.ProcessId}";

void Require(bool condition, string stage)
{
    if (!condition) throw new InvalidOperationException(stage);
}

string Literal(string value) => "'" + value.Replace("'", "''") + "'";

int Execute(DuckDBConnection connection, string sql)
{
    using var command = connection.CreateCommand();
    command.CommandText = sql;
    return command.ExecuteNonQuery();
}

long Number(DuckDBConnection connection, string sql)
{
    using var command = connection.CreateCommand();
    command.CommandText = sql;
    return Convert.ToInt64(command.ExecuteScalar());
}

DuckDBConnection Open(string endpoint)
{
    var connection = new DuckDBConnection("DataSource=:memory:");
    connection.Open();
    try
    {
        Require(Number(connection, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded") == 2,
            "HTTPFS and Quack must be built in and loaded by default");
        Execute(connection, "CONNECT " + Literal(endpoint) + " (TOKEN " + Literal(token) + ")");
        return connection;
    }
    catch { connection.Dispose(); throw; }
}

try
{
    string root = Path.GetFullPath(args[0]), state = Path.GetFullPath(args[1]);
    Require(state.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal), "Descriptor must remain inside root");
    using var descriptor = JsonDocument.Parse(File.ReadAllText(state));
    token = descriptor.RootElement.GetProperty("token").GetString()!;
    string endpoint = descriptor.RootElement.GetProperty("endpoint").GetString()!;
    string nativePath = Path.Combine(root, ".native/lib/libduckdb.dylib");
    if (!File.Exists(nativePath)) nativePath = Path.Combine(root, "lib/libduckdb.dylib");
    IntPtr native = NativeLibrary.Load(nativePath);
    NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly,
        (name, assembly, path) => name == "duckdb" ? native : IntPtr.Zero);
    using var writer = Open(endpoint);
    using var reader = Open(endpoint);
    checks.Add("builtin_httpfs_quack_without_LOAD");
    version = writer.ServerVersion;
    Require(writer.GetType() == typeof(DuckDBConnection), "Official ADO.NET connection type");
    checks.Add("unmodified_official_ADO_NET_shared_native_engine");
    Execute(writer, $"CREATE TABLE {table}(id INTEGER PRIMARY KEY,value BIGINT,label VARCHAR,amount DECIMAL(18,4),payload BLOB,optional VARCHAR)");
    try
    {
        using (var insert = writer.CreateCommand())
        {
            insert.CommandText = $"INSERT INTO {table} VALUES($id,$value,$label,$amount,$payload,$optional)";
            insert.Parameters.Add(new DuckDBParameter("id", 1));
            insert.Parameters.Add(new DuckDBParameter("value", 9007199254740993L));
            insert.Parameters.Add(new DuckDBParameter("label", "C# ñ 🍊"));
            insert.Parameters.Add(new DuckDBParameter("amount", 12345.6700m));
            insert.Parameters.Add(new DuckDBParameter("payload", new byte[] { 0, 1, 255 }));
            insert.Parameters.Add(new DuckDBParameter("optional", DBNull.Value));
            insert.Prepare();
            int count = insert.ExecuteNonQuery();
            Require(count == 1, $"Prepared insertion count: expected 1, received {count}");
        }
        using (var select = reader.CreateCommand())
        {
            select.CommandText = $"SELECT value,label,amount,payload,optional FROM {table} WHERE id=1";
            using var result = select.ExecuteReader();
            Require(result.Read() && result.GetInt64(0) == 9007199254740993L, "Exact Int64 binding");
            Require(result.GetString(1) == "C# ñ 🍊", "Unicode binding");
            Require(result.GetDecimal(2) == 12345.6700m, "Decimal binding");
            using var blob = result.GetStream(3);
            using var payload = new MemoryStream();
            blob.CopyTo(payload);
            Require(payload.ToArray().SequenceEqual(new byte[] { 0, 1, 255 }), "Binary binding through official stream API");
            Require(result.IsDBNull(4), "Null binding");
        }
        checks.Add("native_prepared_named_bindings_types_unicode_decimal_blob_null");
        bool constraint = false;
        try { Execute(writer, $"INSERT INTO {table}(id) VALUES(1)"); }
        catch (DuckDBException expected) { constraint = expected.ErrorType == DuckDBErrorType.Constraint; }
        Require(constraint && Number(writer, $"SELECT count(*) FROM {table}") == 1, "Constraint type and recovery");
        checks.Add("native_constraint_error_and_recovery");
        using (var transaction = writer.BeginTransaction())
        {
            Execute(writer, $"UPDATE {table} SET value=42 WHERE id=1");
            var timer = Stopwatch.StartNew();
            Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 9007199254740993L, "Reader sees committed snapshot");
            Require(timer.Elapsed.TotalSeconds < 2, "Reader does not block behind write transaction");
            transaction.Rollback();
        }
        Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 9007199254740993L, "Rollback result");
        using (var transaction = writer.BeginTransaction())
        {
            Execute(writer, $"UPDATE {table} SET value=43 WHERE id=1"); transaction.Commit();
        }
        Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 43, "Commit result");
        checks.Add("two_sessions_native_ADO_NET_transaction_mvcc_nonblocking_commit_rollback");
        using (var snapshot = reader.BeginTransaction())
        {
            Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 43, "Start reader snapshot");
            Require(Execute(writer, $"UPDATE {table} SET value=44 WHERE id=1") == 1, "Direct update row count");
            Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 43, "Reader transaction retains snapshot after writer commit");
            snapshot.Commit();
        }
        Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 44, "New reader transaction sees writer commit");
        Execute(writer, $"UPDATE {table} SET value=43 WHERE id=1");
        checks.Add("reader_transaction_snapshot_isolation_and_direct_update_count");
        using (var query = reader.CreateCommand())
        {
            query.CommandText = "SELECT i FROM range(15003) AS t(i)";
            using var rows = query.ExecuteReader();
            long count = 0;
            while (rows.Read()) { Require(rows.GetInt64(0) == count, "Ordered multi-vector result"); count++; }
            Require(count == 15003, "Complete multi-vector result");
        }
        checks.Add("streaming_result_15003_rows");
        if (Number(writer, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'") > 0)
        {
            using (var read = reader.CreateCommand())
            {
                read.CommandText = "SELECT id,label FROM lang_multiclient_probe ORDER BY id";
                using var rows = read.ExecuteReader();
                while (rows.Read()) Require(!rows.IsDBNull(1), "Read markers from other languages");
            }
            using (var marker = writer.CreateCommand())
            {
                marker.CommandText = "INSERT INTO lang_multiclient_probe VALUES($id,$label)";
                marker.Parameters.Add(new DuckDBParameter("id", 8));
                marker.Parameters.Add(new DuckDBParameter("label", "from C#"));
                Require(marker.ExecuteNonQuery() == 1, "C# interoperability marker");
            }
            Require(Number(reader, "SELECT count(*) FROM lang_multiclient_probe WHERE id=8 AND label='from C#'") == 1, "Independent reader sees C# marker");
            checks.Add("shared_multilanguage_table_read_and_write");
        }
        using (var abandoned = Open(endpoint))
        {
            Execute(abandoned, "BEGIN"); Execute(abandoned, $"UPDATE {table} SET value=99 WHERE id=1");
            abandoned.Close();
        }
        Require(Number(reader, $"SELECT value FROM {table} WHERE id=1") == 43, "Connection close rollback");
        checks.Add("native_close_rollback");
        Require(Execute(writer, $"DELETE FROM {table} WHERE id=1") == 1 && Number(reader, $"SELECT count(*) FROM {table}") == 0, "Delete count and persistence");
        checks.Add("crud_delete");
        passed = true;
    }
    finally { Execute(writer, $"DROP TABLE {table}"); }
}
catch (Exception exception)
{
    error = exception.GetType().FullName + ": " + exception.Message;
    if (!string.IsNullOrEmpty(token)) error = error.Replace(token, "<redacted>");
}
Console.WriteLine(JsonSerializer.Serialize(new { passed, native_used = true, driver = "DuckDB.NET.Data.DuckDBConnection", version, checks, seconds = stopwatch.Elapsed.TotalSeconds, error }));
return passed ? 0 : 1;
