import java.math.BigDecimal;
import java.sql.*;
import java.util.Properties;

public final class Crud {
    static String env(String name, String fallback) { return System.getenv().getOrDefault(name, fallback); }
    public static void main(String[] args) throws Exception {
        String owner = "PostgreSQL Java";
        Properties properties = new Properties();
        properties.setProperty("user", env("PGUSER", "duckonwire"));
        properties.setProperty("password", env("PGPASSWORD", env("DUCKONWIRE_TOKEN", "")));
        String sslMode = env("PGSSLMODE", "disable");
        if (!sslMode.equals("disable") && !sslMode.equals("verify-full")) throw new IllegalArgumentException("Use PGSSLMODE=disable or verify-full");
        properties.setProperty("sslmode", sslMode);
        if (sslMode.equals("verify-full")) {
            String rootCertificate = env("PGSSLROOTCERT", "");
            if (rootCertificate.isEmpty()) throw new IllegalArgumentException("Set PGSSLROOTCERT for verify-full");
            properties.setProperty("sslrootcert", rootCertificate);
        }
        properties.setProperty("prepareThreshold", "1");
        String url = "jdbc:postgresql://" + env("PGHOST", "127.0.0.1") + ":" + env("PGPORT", "9496") + "/" + env("PGDATABASE", "duckonwire");
        try (Connection connection = DriverManager.getConnection(url, properties)) {
            try (Statement statement = connection.createStatement()) {
                statement.execute("CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))");
            }
            connection.setAutoCommit(false);
            try (PreparedStatement statement = connection.prepareStatement("DELETE FROM pg_demo_products WHERE client=?")) {
                statement.setString(1, owner); statement.executeUpdate();
            }
            try (PreparedStatement insert = connection.prepareStatement("INSERT INTO pg_demo_products VALUES(?,?,?,?,?)")) {
                for (Object[] row : new Object[][] {{1, "Cafe", "19.99", 3}, {2, "Arroz", "8.50", 10}}) {
                    insert.setString(1, owner); insert.setInt(2, (Integer) row[0]);
                    insert.setString(3, (String) row[1]); insert.setBigDecimal(4, new BigDecimal((String) row[2]));
                    insert.setInt(5, (Integer) row[3]); insert.addBatch();
                }
                insert.executeBatch();
            }
            try (PreparedStatement update = connection.prepareStatement("UPDATE pg_demo_products SET stock=stock+1 WHERE client=? AND id=?")) {
                update.setString(1, owner); update.setInt(2, 1); update.executeUpdate();
            }
            try (PreparedStatement delete = connection.prepareStatement("DELETE FROM pg_demo_products WHERE client=? AND id=?")) {
                delete.setString(1, owner); delete.setInt(2, 2); delete.executeUpdate();
            }
            connection.commit(); connection.setAutoCommit(true);
            try (PreparedStatement select = connection.prepareStatement("SELECT id,name,price,stock FROM pg_demo_products WHERE client=? ORDER BY id")) {
                select.setString(1, owner);
                try (ResultSet rows = select.executeQuery()) {
                    if (!rows.next() || rows.getInt(1) != 1 || !rows.getString(2).equals("Cafe") || rows.getBigDecimal(3).compareTo(new BigDecimal("19.99")) != 0 || rows.getInt(4) != 4) throw new SQLException("Unexpected committed row");
                    String price = rows.getBigDecimal(3).toPlainString();
                    String row = "[" + rows.getInt(1) + ",\"" + rows.getString(2) + "\",\"" + price + "\"," + rows.getInt(4) + "]";
                    if (rows.next()) throw new SQLException("Unexpected second row");
                    System.out.println("{\"passed\":true,\"client\":\"" + owner + "\",\"driver\":\"org.postgresql.Driver\",\"rows\":[" + row + "]}");
                }
            }
        }
    }
}
