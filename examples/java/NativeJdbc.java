import java.math.BigDecimal;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Timestamp;
import java.sql.Types;
import java.time.LocalDate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.duckdb.DuckDBConnection;

/** Uses the official JDBC classes and their JNI implementation, with no SQL transport adapter. */
public final class NativeJdbc {
    private static final List<String> checks = new ArrayList<>();
    private static String token = "";

    private static String json(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) out.append(String.format("\\u%04x", (int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }

    private static String field(String text, String key) {
        Matcher matcher = Pattern.compile("\"" + Pattern.quote(key) + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"").matcher(text);
        if (!matcher.find()) throw new IllegalArgumentException("Missing descriptor field: " + key);
        String encoded = matcher.group(1);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < encoded.length(); i++) {
            char c = encoded.charAt(i);
            if (c != '\\') { out.append(c); continue; }
            c = encoded.charAt(++i);
            switch (c) {
                case '"': case '\\': case '/': out.append(c); break;
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'u': out.append((char)Integer.parseInt(encoded.substring(i + 1, i + 5), 16)); i += 4; break;
                default: throw new IllegalArgumentException("Invalid descriptor JSON escape");
            }
        }
        return out.toString();
    }

    private static String literal(String value) { return "'" + value.replace("'", "''") + "'"; }

    private static void execute(Connection con, String sql) throws SQLException {
        try (Statement statement = con.createStatement()) { statement.execute(sql); }
    }

    private static long number(Connection con, String sql) throws SQLException {
        try (Statement statement = con.createStatement(); ResultSet result = statement.executeQuery(sql)) {
            if (!result.next()) throw new AssertionError("Missing scalar result");
            return result.getLong(1);
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static DuckDBConnection open(String endpoint) throws SQLException {
        Properties properties = new Properties();
        properties.setProperty("jdbc_stream_results", "true");
        DuckDBConnection con = (DuckDBConnection)DriverManager.getConnection("jdbc:duckdb:", properties);
        try {
            require(number(con, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded") == 2,
                    "HTTPFS and Quack must be built in and loaded by default");
            execute(con, "CONNECT " + literal(endpoint) + " (TOKEN " + literal(token) + ")");
            return con;
        } catch (SQLException error) { con.close(); throw error; }
    }

    public static void main(String[] args) {
        boolean passed = false;
        String version = "";
        String error = "";
        String table = "codex_java_native_" + ProcessHandle.current().pid();
        long started = System.nanoTime();
        try {
            Path root = Path.of(args[0]).toRealPath();
            Path state = Path.of(args[1]).toRealPath();
            require(state.startsWith(root), "State must remain inside root");
            String descriptor = Files.readString(state);
            token = field(descriptor, "token");
            String endpoint = field(descriptor, "endpoint");
            Class.forName("org.duckdb.DuckDBDriver");
            try (DuckDBConnection writer = open(endpoint); DuckDBConnection reader = open(endpoint)) {
                checks.add("builtin_httpfs_quack_without_LOAD");
                require(writer.getClass() == DuckDBConnection.class, "Expected original official JDBC connection class");
                try (Statement s = writer.createStatement(); ResultSet rows = s.executeQuery("SELECT version()")) {
                    rows.next(); version = rows.getString(1);
                }
                checks.add("official_class_and_native_version");
                execute(writer, "CREATE TABLE " + table + " (id INTEGER PRIMARY KEY, value INTEGER, label VARCHAR, amount DECIMAL(18,4), day DATE, stamp TIMESTAMP, payload BLOB, optional VARCHAR)");
                try {
                    try (PreparedStatement insert = writer.prepareStatement("INSERT INTO " + table + " VALUES (?, ?, ?, ?, ?, ?, ?, ?)")) {
                        insert.setInt(1, 1); insert.setInt(2, 10); insert.setString(3, "Ofertas ñ 🍊");
                        insert.setBigDecimal(4, new BigDecimal("12345.6700"));
                        insert.setDate(5, java.sql.Date.valueOf(LocalDate.of(2026, 10, 8)));
                        insert.setTimestamp(6, Timestamp.valueOf("2026-10-08 12:34:56.123456"));
                        insert.setBytes(7, new byte[]{0, 1, (byte)255}); insert.setNull(8, Types.VARCHAR);
                        require(insert.executeUpdate() == 1, "Prepared insertion count");
                    }
                    try (Statement s = reader.createStatement(); ResultSet r = s.executeQuery("SELECT * FROM " + table + " WHERE id=1")) {
                        require(r.next() && r.getInt(2) == 10, "Read inserted row");
                        require(r.getString(3).equals("Ofertas ñ 🍊"), "Unicode binding");
                        require(r.getBigDecimal(4).equals(new BigDecimal("12345.6700")), "Decimal binding");
                        require(r.getDate(5).toLocalDate().equals(LocalDate.of(2026, 10, 8)), "Date binding");
                        require(r.getTimestamp(6).equals(Timestamp.valueOf("2026-10-08 12:34:56.123456")), "Timestamp binding");
                        require(Arrays.equals(r.getBytes(7), new byte[]{0, 1, (byte)255}), "Binary binding");
                        require(r.getString(8) == null && r.wasNull(), "Null binding");
                    }
                    checks.add("prepared_types_unicode_decimal_date_timestamp_blob_null");
                    try (Statement statement = writer.createStatement()) {
                        require(statement.executeUpdate("UPDATE " + table + " SET value=10 WHERE id=1") == 1, "Direct JDBC changed-row count");
                    }
                    checks.add("direct_statement_update_count");
                    try (PreparedStatement batch = writer.prepareStatement("INSERT INTO " + table + "(id,value) VALUES (?,?)")) {
                        for (int i = 2; i <= 5; i++) { batch.setInt(1, i); batch.setInt(2, i * 10); batch.addBatch(); }
                        require(batch.executeBatch().length == 4, "Native JDBC batch counts");
                    }
                    require(number(reader, "SELECT count(*) FROM " + table) == 5, "Batch persisted");
                    checks.add("execute_batch");
                    boolean failed = false;
                    try { execute(writer, "INSERT INTO " + table + "(id) VALUES(1)"); }
                    catch (SQLException expected) { failed = expected.getMessage().contains("Constraint"); }
                    require(failed, "Native constraint error must reach SQLException");
                    require(number(writer, "SELECT count(*) FROM " + table) == 5, "Error recovery");
                    checks.add("native_constraint_error_and_recovery");
                    writer.setAutoCommit(false);
                    require(!writer.getAutoCommit(), "JDBC transaction mode");
                    execute(writer, "UPDATE " + table + " SET value=99 WHERE id=1");
                    long beforeRead = System.nanoTime();
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 10, "Reader sees prior committed value");
                    require(System.nanoTime() - beforeRead < 2_000_000_000L, "Reader blocked behind open write transaction");
                    writer.rollback();
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 10, "Rollback preserved prior value");
                    execute(writer, "UPDATE " + table + " SET value=42 WHERE id=1"); writer.commit();
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 42, "Explicit commit visible");
                    execute(writer, "UPDATE " + table + " SET value=43 WHERE id=1"); writer.setAutoCommit(true);
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Enabling autocommit commits transaction");
                    checks.add("two_sessions_mvcc_nonblocking_commit_rollback_autocommit");
                    reader.setAutoCommit(false);
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Start reader snapshot");
                    execute(writer, "UPDATE " + table + " SET value=44 WHERE id=1");
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Reader transaction retains snapshot after writer commit");
                    reader.commit(); reader.setAutoCommit(true);
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 44, "New reader transaction sees writer commit");
                    execute(writer, "UPDATE " + table + " SET value=43 WHERE id=1");
                    checks.add("reader_transaction_snapshot_isolation");
                    try (DuckDBConnection duplicate = writer.duplicate()) {
                        require(duplicate.getClass() == DuckDBConnection.class, "Duplicate class");
                        require(number(duplicate, "SELECT value FROM " + table + " WHERE id=1") == 43, "Duplicate inherits native CONNECT");
                        duplicate.setAutoCommit(false); execute(duplicate, "UPDATE " + table + " SET value=77 WHERE id=1");
                        require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Independent duplicate session");
                    }
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Duplicate close rolls back");
                    checks.add("official_duplicate_independent_session_close_rollback");
                    try (Statement s = reader.createStatement(); ResultSet rows = s.executeQuery("SELECT i FROM range(15003) AS t(i)")) {
                        int count = 0;
                        while (rows.next()) { require(rows.getLong(1) == count, "Ordered multi-vector result"); count++; }
                        require(count == 15003, "Complete multi-vector result");
                    }
                    checks.add("streaming_result_15003_rows");
                    if (number(writer, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'") > 0) {
                        try (Statement s = reader.createStatement(); ResultSet rows = s.executeQuery("SELECT id,label FROM lang_multiclient_probe ORDER BY id")) {
                            while (rows.next()) require(rows.getString(2) != null, "Read markers from other languages");
                        }
                        try (PreparedStatement marker = writer.prepareStatement("INSERT INTO lang_multiclient_probe VALUES(?,?)")) {
                            marker.setInt(1, 6); marker.setString(2, "from Java");
                            require(marker.executeUpdate() == 1, "Java interoperability marker");
                        }
                        require(number(reader, "SELECT count(*) FROM lang_multiclient_probe WHERE id=6 AND label='from Java'") == 1, "Independent reader sees Java marker");
                        checks.add("shared_multilanguage_table_read_and_write");
                    }
                    DuckDBConnection abandoned = open(endpoint);
                    abandoned.setAutoCommit(false);
                    PreparedStatement held = abandoned.prepareStatement("UPDATE " + table + " SET value=? WHERE id=1");
                    held.setInt(1, 88); held.executeUpdate(); abandoned.close();
                    require(held.isClosed(), "Official connection close releases retained statement");
                    require(number(reader, "SELECT value FROM " + table + " WHERE id=1") == 43, "Connection close rolls back pending write");
                    checks.add("close_retained_statement_rollback");
                    execute(writer, "DELETE FROM " + table + " WHERE id=5");
                    require(number(reader, "SELECT count(*) FROM " + table) == 4, "Delete committed");
                    checks.add("crud_delete");
                    passed = true;
                } finally { writer.close(); execute(reader, "DROP TABLE IF EXISTS " + table); }
            }
        } catch (Throwable exception) {
            error = exception.getClass().getName() + ": " + exception.getMessage();
            if (!token.isEmpty()) error = error.replace(token, "<redacted>");
        }
        String checkJson = checks.stream().map(NativeJdbc::json).reduce((a, b) -> a + "," + b).orElse("");
        System.out.println("{\"passed\":" + passed + ",\"native_used\":true,\"driver\":\"org.duckdb.DuckDBConnection\",\"version\":" + json(version)
            + ",\"checks\":[" + checkJson + "],\"seconds\":" + ((System.nanoTime() - started) / 1e9) + ",\"error\":" + json(error) + "}");
        if (!passed) System.exit(1);
    }
}
