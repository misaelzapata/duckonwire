// Ordinary CRUD with the official duckdb crate and native CONNECT.
use duckdb::{Connection, params};
use serde_json::{Value, json};
use std::error::Error;

type Result<T> = std::result::Result<T, Box<dyn Error>>;
fn literal(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

fn crud(endpoint: &str, token: &str) -> Result<Value> {
    let mut connection = Connection::open_in_memory()?;
    connection.execute_batch(&format!(
        "CONNECT {} (TOKEN {})",
        literal(endpoint),
        literal(token)
    ))?;
    connection.execute_batch("CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))")?;

    let transaction = connection.transaction()?;
    // Repeating this example replaces only this client's products.
    transaction.execute("DELETE FROM demo_products WHERE client=?", ["Rust"])?;
    for (id, name, price, stock) in [(1, "Cafe", "19.99", 3), (2, "Arroz", "8.50", 10)] {
        transaction.execute(
            "INSERT INTO demo_products VALUES(?,?,?,?,?)",
            params!["Rust", id, name, price, stock],
        )?;
    }
    transaction.execute(
        "UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?",
        params!["Rust", 1],
    )?;
    {
        let mut statement = transaction.prepare("SELECT id,name,CAST(price AS VARCHAR),stock FROM demo_products WHERE client=? ORDER BY id")?;
        let products: Vec<(i32, String, String, i32)> = statement
            .query_map(["Rust"], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
            })?
            .collect::<std::result::Result<_, _>>()?;
        if products.len() != 2 {
            return Err("expected both products before DELETE".into());
        }
    }
    transaction.execute(
        "DELETE FROM demo_products WHERE client=? AND id=?",
        params!["Rust", 2],
    )?;
    transaction.commit()?;

    let product: (i32, String, String, i32) = connection.query_row(
        "SELECT id,name,CAST(price AS VARCHAR),stock FROM demo_products WHERE client=? AND id=?",
        params!["Rust", 1],
        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
    )?;
    if product != (1, "Cafe".into(), "19.99".into(), 4) {
        return Err("committed product does not match CRUD operations".into());
    }
    Ok(json!([[product.0, product.1, product.2, product.3]]))
}

fn main() {
    let endpoint = std::env::var("DUCKONWIRE_ENDPOINT")
        .ok()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "quack:127.0.0.1:9495".into());
    let token = std::env::var("DUCKONWIRE_TOKEN").unwrap_or_default();
    let result: Result<Value> = (|| {
        if token.is_empty() {
            return Err("DUCKONWIRE_TOKEN is required".into());
        }
        crud(&endpoint, &token)
    })();
    match result {
        Ok(rows) => println!("{}", json!({"passed":true,"language":"Rust","rows":rows})),
        Err(error) => {
            let message = if token.is_empty() {
                error.to_string()
            } else {
                error.to_string().replace(&token, "[REDACTED]")
            };
            println!(
                "{}",
                json!({"passed":false,"language":"Rust","error":message})
            );
            std::process::exit(1);
        }
    }
}
