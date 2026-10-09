import java.math.BigDecimal;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

/** A normal JDBC transaction using the native DuckDB driver. */
public final class NormalCrud {
    private static final String CLIENT = "Java";

    private static String literal(String text) { return "'" + text.replace("'", "''") + "'"; }

    private static String rows(Connection connection) throws Exception {
        List<String> rows = new ArrayList<>();
        try (PreparedStatement select = connection.prepareStatement(
                "SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id")) {
            select.setString(1, CLIENT);
            try (ResultSet result = select.executeQuery()) {
                while (result.next()) rows.add("[" + result.getInt(1) + ",\"" +
                    result.getString(2) + "\",\"" + result.getBigDecimal(3).toPlainString() +
                    "\"," + result.getInt(4) + "]");
            }
        }
        return "[" + String.join(",", rows) + "]";
    }

    public static void main(String[] args) {
        String token = "";
        try {
            String endpoint = System.getenv().getOrDefault("DUCKONWIRE_ENDPOINT", "quack:127.0.0.1:9495");
            token = System.getenv().getOrDefault("DUCKONWIRE_TOKEN", "");
            if (token.isBlank()) throw new IllegalArgumentException("DUCKONWIRE_TOKEN must be set");
            try (Connection connection = DriverManager.getConnection("jdbc:duckdb:");
                 Statement sql = connection.createStatement()) {
                sql.execute("CONNECT " + literal(endpoint) + " (TOKEN " + literal(token) + ")");
                sql.execute("CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))");
                connection.setAutoCommit(false);
                try (PreparedStatement clear = connection.prepareStatement("DELETE FROM demo_products WHERE client=?");
                     PreparedStatement insert = connection.prepareStatement("INSERT INTO demo_products VALUES(?,?,?,?,?)");
                     PreparedStatement update = connection.prepareStatement("UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?");
                     PreparedStatement delete = connection.prepareStatement("DELETE FROM demo_products WHERE client=? AND id=?")) {
                    clear.setString(1, CLIENT); clear.executeUpdate();
                    String[] names = {"Cafe", "Arroz"}, prices = {"19.99", "8.50"};
                    int[] stocks = {3, 10};
                    for (int index = 0; index < 2; index++) {
                        insert.setString(1, CLIENT); insert.setInt(2, index + 1); insert.setString(3, names[index]);
                        insert.setBigDecimal(4, new BigDecimal(prices[index])); insert.setInt(5, stocks[index]);
                        if (insert.executeUpdate() != 1) throw new IllegalStateException("Insertion count");
                    }
                    update.setString(1, CLIENT); update.setInt(2, 1); update.executeUpdate();
                    String before = rows(connection);
                    delete.setString(1, CLIENT); delete.setInt(2, 2); delete.executeUpdate();
                    connection.commit();
                    connection.setAutoCommit(true);
                    String after = rows(connection);
                    if (!after.equals("[[1,\"Cafe\",\"19.99\",4]]"))
                        throw new IllegalStateException("Persisted product does not match");
                    System.out.println("{\"passed\":true,\"client\":\"Java\",\"before_delete\":" + before + ",\"rows\":" + after + "}");
                }
            }
        } catch (Exception error) {
            String message = error.toString().replace(token.isEmpty() ? "\u0000" : token, "<redacted>")
                .replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "\\r");
            System.out.println("{\"passed\":false,\"client\":\"Java\",\"error\":\"" + message + "\"}");
            System.exit(1);
        }
    }
}
