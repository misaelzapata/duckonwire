// Official duckdb crate, dynamically linked to the existing native engine.
use duckdb::types::{Decimal, Value};
use duckdb::{Config, Connection, named_params, params};
use serde::{Deserialize, Serialize};
use std::{
    error::Error,
    fs,
    path::Path,
    time::{Duration, Instant},
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;
#[derive(Deserialize, Default)]
struct State {
    #[serde(default)]
    uri: String,
    #[serde(default)]
    endpoint: String,
    token: String,
}
#[derive(Clone, Serialize)]
struct BuiltinExtension {
    extension_name: String,
    loaded: bool,
    installed: bool,
    install_mode: String,
    install_path: String,
}
#[derive(Serialize)]
struct Check {
    name: String,
    passed: bool,
    milliseconds: f64,
}
#[derive(Serialize)]
struct Report {
    passed: bool,
    native_used: bool,
    driver: &'static str,
    native_version: String,
    client_library_version: String,
    checks: Vec<Check>,
    limits: Vec<&'static str>,
    #[serde(skip_serializing_if = "String::is_empty")]
    error: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    stage: String,
    multiclient_shared_state: bool,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    multiclient_rows_seen: Vec<SharedRow>,
    driver_autocommit_inside_transaction: Option<bool>,
    builtin_extensions_before_connect: Vec<BuiltinExtension>,
}
#[derive(Serialize)]
struct SharedRow {
    id: i32,
    label: String,
}
fn literal(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}
fn require(ok: bool, message: &str) -> Result<()> {
    if ok {
        Ok(())
    } else {
        Err(std::io::Error::other(message).into())
    }
}
fn open(
    root: &Path,
    state: &State,
    proof: Option<&mut Vec<BuiltinExtension>>,
) -> Result<Connection> {
    let standalone = root.join("bin/duckonwire-server").is_file();
    let temporary = root.join(if standalone {
        "data/tmp-rust"
    } else {
        ".native/languages/rust/tmp"
    });
    fs::create_dir_all(&temporary)?;
    let configuration = Config::default().threads(2)?.with(
        "temp_directory",
        temporary.to_str().ok_or("invalid temp path")?,
    )?;
    let connection = Connection::open_in_memory_with_flags(configuration)?;
    let extensions = {
        let mut statement = connection.prepare("SELECT extension_name,loaded,installed,install_mode,install_path FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') ORDER BY extension_name")?;
        let mut rows = statement.query([])?;
        let mut extensions = Vec::new();
        while let Some(row) = rows.next()? {
            extensions.push(BuiltinExtension {
                extension_name: row.get(0)?,
                loaded: row.get(1)?,
                installed: row.get(2)?,
                install_mode: row.get(3)?,
                install_path: row.get(4)?,
            });
        }
        extensions
    };
    if let Some(proof) = proof {
        *proof = extensions.clone();
    }
    require(
        extensions.len() == 2,
        "httpfs and quack must be built in before CONNECT",
    )?;
    for (extension, expected) in extensions.iter().zip(["httpfs", "quack"]) {
        require(
            extension.extension_name == expected
                && extension.loaded
                && extension.installed
                && extension.install_mode == "STATICALLY_LINKED"
                && extension.install_path == "(BUILT-IN)",
            &format!("extension {expected} is not loaded as a native built-in before CONNECT"),
        )?;
    }
    connection.execute_batch(&format!(
        "CONNECT {} (TOKEN {})",
        literal(&state.uri),
        literal(&state.token)
    ))?;
    Ok(connection)
}
fn scalar(connection: &Connection, query: &str) -> Result<i64> {
    Ok(connection.query_row(query, [], |row| row.get(0))?)
}

fn run(root: &Path, state: &State, report: &mut Report) -> Result<()> {
    report.stage = "builtin_extensions_loaded_before_connect".into();
    let start = Instant::now();
    let connection = open(
        root,
        state,
        Some(&mut report.builtin_extensions_before_connect),
    );
    report.checks.push(Check {
        name: report.stage.clone(),
        passed: connection.is_ok(),
        milliseconds: start.elapsed().as_secs_f64() * 1000.0,
    });
    let mut connection = connection?;
    let mut other = open(root, state, None)?;
    let table = format!("codex_native_rust_{}", std::process::id());
    let bulk = format!("{table}_bulk");
    macro_rules! gate {
        ($name:expr,$body:expr) => {{
            report.stage = $name.to_string();
            let start = Instant::now();
            let result: Result<()> = (|| $body)();
            report.checks.push(Check {
                name: $name.to_string(),
                passed: result.is_ok(),
                milliseconds: start.elapsed().as_secs_f64() * 1000.0,
            });
            result?;
        }};
    }
    gate!("official_driver_native_identity", {
        report.native_version = connection.version()?;
        report.client_library_version =
            unsafe { std::ffi::CStr::from_ptr(duckdb::ffi::duckdb_library_version()) }
                .to_str()?
                .to_string();
        report.native_used = report.native_version == "v2.1.0-dev0"
            && report.client_library_version == report.native_version;
        require(report.native_used, "custom native engine was not loaded")
    });
    gate!("ddl_schema_freshness_independent_connections", {
        connection.execute_batch(&format!("CREATE TABLE {table}(id INTEGER PRIMARY KEY,val BIGINT,label VARCHAR,amount DECIMAL(18,4),raw BLOB,nums INTEGER[],nullable INTEGER,nest STRUCT(label VARCHAR,v BIGINT))"))?;
        require(
            scalar(&other, &format!("SELECT count(*) FROM {table}"))? == 0,
            "fresh DDL missing from independent connection",
        )
    });
    gate!("positional_typed_decimal_blob_null_list_struct_sql_cast", {
        connection.execute(&format!("INSERT INTO {table} VALUES(?,?,?,?,?,CAST(? AS INTEGER[]),?,CAST(? AS STRUCT(label VARCHAR,v BIGINT)))"),
            params![1i32,9i64,"ñ-Rust-🙂",Decimal::new(18,4,1234500)?,vec![0u8,1,127,255],"[1,2,3]",None::<i32>,"{'label':'struct-Rust','v':42}"])?;
        Ok(())
    });
    gate!("native_complex_results_preserve_values", {
        let result = other.query_row(
            &format!("SELECT * FROM {table} WHERE id=?"),
            [1i32],
            |row| {
                Ok((
                    row.get::<_, i32>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, Decimal>(3)?,
                    row.get::<_, Vec<u8>>(4)?,
                    row.get::<_, Value>(5)?,
                    row.get::<_, Option<i32>>(6)?,
                    row.get::<_, Value>(7)?,
                ))
            },
        )?;
        require(
            result.0 == 1
                && result.1 == 9
                && result.2 == "ñ-Rust-🙂"
                && result.3.width() == 18
                && result.3.scale() == 4
                && result.3.value() == 1234500
                && result.4 == vec![0u8, 1, 127, 255]
                && result.6.is_none(),
            "scalar/decimal/blob/null result mismatch",
        )?;
        require(
            result.5 == Value::List(vec![Value::Int(1), Value::Int(2), Value::Int(3)]),
            "LIST result mismatch",
        )?;
        if let Value::Struct(fields) = result.7 {
            require(
                fields.get(&"label".to_string()) == Some(&Value::Text("struct-Rust".to_string()))
                    && fields.get(&"v".to_string()) == Some(&Value::BigInt(42)),
                "STRUCT result mismatch",
            )
        } else {
            require(false, "STRUCT result lost native type")
        }
    });
    gate!("named_repeated_prepared_parameters", {
        let mut statement =
            connection.prepare("SELECT $value::BIGINT+$value::BIGINT,$label::VARCHAR")?;
        let (value, label) = statement
            .query_row(named_params! {"value":7i64,"label":"named"}, |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
            })?;
        require(
            value == 14 && label == "named",
            "named/repeated parameter mismatch",
        )
    });
    gate!("prepared_reuse_null_first_bulk_crud", {
        connection.execute_batch(&format!(
            "CREATE TABLE {bulk}(id INTEGER PRIMARY KEY,note VARCHAR)"
        ))?;
        {
            let mut statement = connection.prepare(&format!("INSERT INTO {bulk} VALUES(?,?)"))?;
            for id in 0..128i32 {
                let note = if id == 0 {
                    None
                } else {
                    Some(format!("row-{id}"))
                };
                let changed = statement.execute(params![id, note])?;
                require(
                    changed == 1,
                    &format!("prepared insert changed rowcount {changed}, expected 1"),
                )?;
            }
        }
        connection.execute(
            &format!("UPDATE {bulk} SET note=? WHERE id=?"),
            params!["updated", 1i32],
        )?;
        connection.execute(&format!("DELETE FROM {bulk} WHERE id=?"), [127i32])?;
        require(
            scalar(&other, &format!("SELECT count(*) FROM {bulk}"))? == 127,
            "bulk CRUD mismatch",
        )
    });
    gate!("native_errors_recovery_and_driver_container_limit", {
        let duplicate = connection
            .execute(&format!("INSERT INTO {bulk} VALUES(0,'duplicate')"), [])
            .unwrap_err();
        match duplicate {
            duckdb::Error::DuckDBFailure(error, Some(message)) => require(
                error.extended_code == duckdb::ffi::DuckDBError
                    && message.starts_with("Constraint Error:"),
                "constraint error classification mismatch",
            )?,
            _ => return Err("constraint did not return native error".into()),
        };
        require(
            connection.execute("SELECT FROM", []).is_err(),
            "invalid SQL accepted",
        )?;
        let unsupported = connection.query_row(
            "SELECT ?::INTEGER[]",
            params![Value::List(vec![Value::Int(1)])],
            |row| row.get::<_, Value>(0),
        );
        require(
            unsupported.is_err(),
            "official container parameter support unexpectedly changed",
        )?;
        require(
            scalar(&connection, "SELECT 41::BIGINT")? == 41,
            "errors prevented connection recovery",
        )
    });
    gate!("iterative_result_paging_15003_rows", {
        let mut statement =
            connection.prepare("SELECT i::BIGINT AS value FROM range(15003) t(i)")?;
        let mut rows = statement.query([])?;
        let mut count = 0i64;
        while let Some(row) = rows.next()? {
            require(row.get::<_, i64>(0)? == count, "paged row mismatch")?;
            count += 1;
        }
        drop(rows);
        require(
            statement.column_names() == vec!["value".to_string()],
            "result metadata mismatch",
        )?;
        require(count == 15003, "paged result truncated")
    });
    gate!(
        "transactions_snapshot_nonblocking_reader_commit_rollback",
        {
            let reader = other.transaction()?;
            report.driver_autocommit_inside_transaction = Some(reader.is_autocommit());
            require(
                scalar(&reader, &format!("SELECT val FROM {table} WHERE id=1"))? == 9,
                "initial snapshot mismatch",
            )?;
            let writer = connection.transaction()?;
            writer.execute(&format!("UPDATE {table} SET val=10 WHERE id=1"), [])?;
            let started = Instant::now();
            require(
                scalar(&reader, &format!("SELECT val FROM {table} WHERE id=1"))? == 9
                    && started.elapsed() < Duration::from_secs(2),
                "reader blocked or saw uncommitted value",
            )?;
            writer.commit()?;
            require(
                scalar(&reader, &format!("SELECT val FROM {table} WHERE id=1"))? == 9,
                "snapshot changed on external commit",
            )?;
            reader.commit()?;
            require(
                scalar(&other, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
                "committed value missing",
            )?;
            let rollback = connection.transaction()?;
            rollback.execute(&format!("UPDATE {table} SET val=999 WHERE id=1"), [])?;
            rollback.rollback()?;
            require(
                scalar(&other, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
                "rollback leaked mutation",
            )
        }
    );
    gate!("mvcc_conflicting_writers_recover", {
        let first = connection.transaction()?;
        let second = other.transaction()?;
        require(
            scalar(&first, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
            "first writer initial snapshot mismatch",
        )?;
        require(
            scalar(&second, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
            "second writer initial snapshot mismatch",
        )?;
        first.execute(&format!("UPDATE {table} SET val=11 WHERE id=1"), [])?;
        let conflict = second
            .execute(&format!("UPDATE {table} SET val=12 WHERE id=1"), [])
            .unwrap_err();
        match conflict {
            duckdb::Error::DuckDBFailure(error, Some(message)) => require(
                error.extended_code == duckdb::ffi::DuckDBError
                    && message.starts_with("TransactionContext Error:")
                    && message.contains("Conflict on update"),
                "conflict native error type mismatch",
            )?,
            _ => return Err("conflict did not return native DuckDBFailure".into()),
        }
        second.rollback()?;
        first.commit()?;
        require(
            scalar(&other, &format!("SELECT val FROM {table} WHERE id=1"))? == 11,
            "winning writer state lost",
        )?;
        connection.execute(&format!("UPDATE {table} SET val=10 WHERE id=1"), [])?;
        Ok(())
    });
    gate!("transaction_drop_and_connection_close_roll_back", {
        {
            let transaction = connection.transaction()?;
            transaction.execute(&format!("UPDATE {table} SET val=555 WHERE id=1"), [])?;
        }
        require(
            scalar(&other, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
            "transaction Drop did not rollback",
        )?;
        {
            let pending = open(root, state, None)?;
            pending.execute_batch(&format!("BEGIN; UPDATE {table} SET val=666 WHERE id=1"))?;
            pending.close().map_err(|(_, error)| error)?;
        }
        require(
            scalar(&other, &format!("SELECT val FROM {table} WHERE id=1"))? == 10,
            "connection close did not rollback",
        )
    });
    if scalar(
        &connection,
        "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'",
    )? > 0
    {
        gate!("multiclient_shared_table", {
            let mut statement =
                connection.prepare("SELECT id,label FROM lang_multiclient_probe ORDER BY id")?;
            let mut rows = statement.query([])?;
            while let Some(row) = rows.next()? {
                report.multiclient_rows_seen.push(SharedRow {
                    id: row.get(0)?,
                    label: row.get(1)?,
                });
            }
            drop(rows);
            drop(statement);
            require(
                !report.multiclient_rows_seen.is_empty(),
                "previous language rows missing",
            )?;
            connection.execute(
                "INSERT INTO lang_multiclient_probe VALUES(?,?)",
                params![5i32, "from Rust"],
            )?;
            let count: i64 = other.query_row(
                "SELECT count(*) FROM lang_multiclient_probe WHERE id=? AND label=?",
                params![5i32, "from Rust"],
                |row| row.get(0),
            )?;
            report.multiclient_shared_state = count == 1;
            require(
                report.multiclient_shared_state,
                "Rust row missing from independent connection",
            )
        });
    }
    connection.execute_batch(&format!("DROP TABLE {bulk}; DROP TABLE {table}"))?;
    report.stage.clear();
    report.passed = true;
    Ok(())
}
fn main() {
    let mut report = Report {
        passed: false,
        native_used: false,
        driver: "duckdb@1.10506.0",
        native_version: String::new(),
        client_library_version: String::new(),
        checks: vec![],
        limits: vec![
            "The official crate does not bind LIST/STRUCT Value parameters; bound text is cast by DuckDB to native LIST/STRUCT, and result values preserve those types.",
            "The pinned official crate hardcodes is_autocommit() to true even inside an explicit transaction. SQL and transaction commit/rollback/Drop are validated independently.",
            "The pinned official crate maps execution failures to DuckDBFailure(state=1, ErrorCode::Unknown), retaining the native message but discarding native exception subtypes; errors and recovery are tested against this API.",
            "Validated only on macOS arm64 with the pinned native engine and Quack protocol 7.",
        ],
        error: String::new(),
        stage: String::new(),
        multiclient_shared_state: false,
        multiclient_rows_seen: vec![],
        driver_autocommit_inside_transaction: None,
        builtin_extensions_before_connect: vec![],
    };
    let mut state = State::default();
    let result: Result<()> = (|| {
        let arguments: Vec<String> = std::env::args().collect();
        require(
            arguments.len() == 3,
            "usage: native-rust-client ROOT STATE_JSON",
        )?;
        let root = fs::canonicalize(&arguments[1])?;
        state = serde_json::from_slice(&fs::read(&arguments[2])?)?;
        if state.uri.is_empty() {
            state.uri = state.endpoint.clone()
        };
        require(
            !state.uri.is_empty() && !state.token.is_empty(),
            "incomplete private native descriptor",
        )?;
        run(&root, &state, &mut report)
    })();
    if let Err(error) = result {
        report.error = error.to_string();
        if !state.token.is_empty() {
            report.error = report.error.replace(&state.token, "[REDACTED]")
        };
    }
    println!(
        "{}",
        serde_json::to_string(&report).expect("serialize result")
    );
    if !report.passed {
        std::process::exit(1)
    }
}
