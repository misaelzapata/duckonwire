use std::env;
use tokio_postgres::{Config, NoTls, config::SslMode};
use postgres_native_tls::MakeTlsConnector;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut config=Config::new();
    config.host(&env::var("PGHOST").unwrap_or("127.0.0.1".into()));
    config.port(env::var("PGPORT").unwrap_or("9496".into()).parse()?);
    config.user(&env::var("PGUSER").unwrap_or("duckonwire".into()));
    config.dbname(&env::var("PGDATABASE").unwrap_or("duckonwire".into()));
    config.password(env::var("PGPASSWORD").or_else(|_|env::var("DUCKONWIRE_TOKEN"))?);
    let mode=env::var("PGSSLMODE").unwrap_or("disable".into());
    let (mut client,connection_task)=match mode.as_str(){
        "disable"=>{
            config.ssl_mode(SslMode::Disable);
            let (client,connection)=config.connect(NoTls).await?;
            (client,tokio::spawn(async move{connection.await}))
        },
        "verify-full"=>{
            let certificate=std::fs::read(env::var("PGSSLROOTCERT")?)?;
            let mut tls=native_tls::TlsConnector::builder();
            tls.add_root_certificate(native_tls::Certificate::from_pem(&certificate)?);
            config.ssl_mode(SslMode::Require);
            let (client,connection)=config.connect(MakeTlsConnector::new(tls.build()?)).await?;
            (client,tokio::spawn(async move{connection.await}))
        },
        _=>return Err("Use PGSSLMODE=disable or verify-full".into()),
    };
    let owner="PostgreSQL Rust";
    client.batch_execute("CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))").await?;
    let transaction=client.transaction().await?;
    transaction.execute("DELETE FROM pg_demo_products WHERE client=$1",&[&owner]).await?;
    let insert=transaction.prepare("INSERT INTO pg_demo_products VALUES($1,$2,$3,CAST($4 AS VARCHAR)::DECIMAL(18,2),$5)").await?;
    for (id,name,price,stock) in [(1i32,"Coffee","19.99",3i32),(2,"Rice","8.50",10)] {
        transaction.execute(&insert,&[&owner,&id,&name,&price,&stock]).await?;
    }
    transaction.execute("UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2",&[&owner,&1i32]).await?;
    transaction.execute("DELETE FROM pg_demo_products WHERE client=$1 AND id=$2",&[&owner,&2i32]).await?;
    transaction.commit().await?;
    let rows=client.query("SELECT id,name,CAST(price AS VARCHAR),stock FROM pg_demo_products WHERE client=$1 ORDER BY id",&[&owner]).await?;
    let selected:Vec<_>=rows.iter().map(|r|serde_json::json!([r.get::<_,i32>(0),r.get::<_,String>(1),r.get::<_,String>(2),r.get::<_,i32>(3)])).collect();
    if selected != vec![serde_json::json!([1,"Coffee","19.99",4])] { return Err("Unexpected committed row".into()); }
    println!("{}",serde_json::json!({"passed":true,"client":owner,"driver":"tokio-postgres","rows":selected}));
    drop(client);connection_task.await??;Ok(())
}
