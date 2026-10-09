using System.Globalization;
using System.Text.Json;
using DuckDB.NET.Data;

const string Client = "C#";
string token = "";
var jsonOptions = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
try
{
    string endpoint = Environment.GetEnvironmentVariable("DUCKONWIRE_ENDPOINT") ?? "quack:127.0.0.1:9495";
    token = Environment.GetEnvironmentVariable("DUCKONWIRE_TOKEN") ?? "";
    if (string.IsNullOrWhiteSpace(token)) throw new ArgumentException("DUCKONWIRE_TOKEN must be set");
    using var connection = new DuckDBConnection("DataSource=:memory:");
    connection.Open();
    int Change(string sql, params (string Name, object Value)[] parameters)
    {
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        foreach (var parameter in parameters) command.Parameters.Add(new DuckDBParameter(parameter.Name, parameter.Value));
        return command.ExecuteNonQuery();
    }
    List<Product> Rows()
    {
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT id,name,price,stock FROM demo_products WHERE client=$client ORDER BY id";
        command.Parameters.Add(new DuckDBParameter("client", Client));
        using var reader = command.ExecuteReader();
        var rows = new List<Product>();
        while (reader.Read()) rows.Add(new(reader.GetInt32(0), reader.GetString(1),
            reader.GetDecimal(2).ToString("0.00", CultureInfo.InvariantCulture), reader.GetInt32(3)));
        return rows;
    }
    string Literal(string value) => "'" + value.Replace("'", "''") + "'";
    Change("CONNECT " + Literal(endpoint) + " (TOKEN " + Literal(token) + ")");
    Change("CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))");
    using var transaction = connection.BeginTransaction();
    Change("DELETE FROM demo_products WHERE client=$client", ("client", Client));
    foreach (var product in new[] { new Product(1, "Cafe", "19.99", 3), new Product(2, "Arroz", "8.50", 10) })
        if (Change("INSERT INTO demo_products VALUES($client,$id,$name,$price,$stock)",
            ("client", Client), ("id", product.Id), ("name", product.Name),
            ("price", decimal.Parse(product.Price, CultureInfo.InvariantCulture)), ("stock", product.Stock)) != 1)
            throw new InvalidOperationException("Insertion count");
    Change("UPDATE demo_products SET stock=stock+1 WHERE client=$client AND id=$id", ("client", Client), ("id", 1));
    var before = Rows();
    Change("DELETE FROM demo_products WHERE client=$client AND id=$id", ("client", Client), ("id", 2));
    transaction.Commit();
    var rows = Rows();
    if (rows.Count != 1 || rows[0] != new Product(1, "Cafe", "19.99", 4))
        throw new InvalidOperationException("Persisted product does not match");
    object[][] Arrays(List<Product> products) => products.Select(product =>
        new object[] { product.Id, product.Name, product.Price, product.Stock }).ToArray();
    Console.WriteLine(JsonSerializer.Serialize(new { passed = true, client = Client, before_delete = Arrays(before), rows = Arrays(rows) }, jsonOptions));
}
catch (Exception error)
{
    string message = error.ToString();
    if (token.Length > 0) message = message.Replace(token, "<redacted>");
    Console.WriteLine(JsonSerializer.Serialize(new { passed = false, client = Client, error = message }, jsonOptions));
    Environment.ExitCode = 1;
}

record Product(int Id, string Name, string Price, int Stock);
