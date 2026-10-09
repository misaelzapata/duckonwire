using System.Globalization;
using System.Text.Json;
using Npgsql;

string Env(string name,string fallback) => Environment.GetEnvironmentVariable(name) ?? fallback;
var sslMode=Env("PGSSLMODE","disable");
if(sslMode!="disable"&&sslMode!="verify-full") throw new ArgumentException("Use PGSSLMODE=disable or verify-full");
if(sslMode=="verify-full"&&string.IsNullOrEmpty(Env("PGSSLROOTCERT",""))) throw new ArgumentException("Set PGSSLROOTCERT for verify-full");
var configuration=new NpgsqlConnectionStringBuilder {
    Host=Env("PGHOST","127.0.0.1"),Port=int.Parse(Env("PGPORT","9496")),
    Username=Env("PGUSER","duckonwire"),Database=Env("PGDATABASE","duckonwire"),
    Password=Env("PGPASSWORD",Env("DUCKONWIRE_TOKEN","")),SslMode=sslMode=="verify-full"?SslMode.VerifyFull:SslMode.Disable,
    RootCertificate=Env("PGSSLROOTCERT",""),
};
await using var connection=new NpgsqlConnection(configuration.ConnectionString);
await connection.OpenAsync();
const string owner="PostgreSQL C#";
await using (var create=new NpgsqlCommand("CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))",connection)) await create.ExecuteNonQueryAsync();
await using (var transaction=await connection.BeginTransactionAsync()) {
    await using (var delete=new NpgsqlCommand("DELETE FROM pg_demo_products WHERE client=$1",connection,transaction)) {
        delete.Parameters.AddWithValue(owner);await delete.ExecuteNonQueryAsync();
    }
    await using (var insert=new NpgsqlCommand("INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)",connection,transaction)) {
        insert.Parameters.AddWithValue(owner);insert.Parameters.AddWithValue(0);insert.Parameters.AddWithValue("");insert.Parameters.AddWithValue(0m);insert.Parameters.AddWithValue(0);
        await insert.PrepareAsync();
        foreach(var row in new[]{(Id:1,Name:"Cafe",Price:19.99m,Stock:3),(Id:2,Name:"Arroz",Price:8.50m,Stock:10)}) {
            insert.Parameters[1].Value=row.Id;insert.Parameters[2].Value=row.Name;insert.Parameters[3].Value=row.Price;insert.Parameters[4].Value=row.Stock;await insert.ExecuteNonQueryAsync();
        }
    }
    foreach(var sql in new[]{"UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2","DELETE FROM pg_demo_products WHERE client=$1 AND id=$2"}) {
        await using var command=new NpgsqlCommand(sql,connection,transaction);command.Parameters.AddWithValue(owner);command.Parameters.AddWithValue(sql.StartsWith("UPDATE")?1:2);await command.ExecuteNonQueryAsync();
    }
    await transaction.CommitAsync();
}
var selected=new List<object[]>();
await using(var select=new NpgsqlCommand("SELECT id,name,price,stock FROM pg_demo_products WHERE client=$1 ORDER BY id",connection)) {
    select.Parameters.AddWithValue(owner);await using var rows=await select.ExecuteReaderAsync();
    while(await rows.ReadAsync()) selected.Add(new object[]{rows.GetInt32(0),rows.GetString(1),rows.GetDecimal(2).ToString("0.00",CultureInfo.InvariantCulture),rows.GetInt32(3)});
}
if(selected.Count!=1||!selected[0].SequenceEqual(new object[]{1,"Cafe","19.99",4})) throw new Exception("Unexpected committed row");
Console.WriteLine(JsonSerializer.Serialize(new{passed=true,client=owner,driver="Npgsql",rows=selected}));
