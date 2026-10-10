# Query DoW from Neon

Neon can query your DuckOnWire (DoW) database without copying its tables. You connect your app to Neon as usual, and Neon reads the DoW tables when your query needs them. You can also join those tables with data stored in Neon.

This guide sets up **read-only access from Neon**. Apps connected directly to DoW can read and write through the same connection when their account has `write` access.

## 1. Prepare the DoW server

Use a current source build with Neon support. Your DoW server needs:

- A hostname and PostgreSQL port reachable from Neon, such as `db.example.com:9496`.
- A TLS certificate for that hostname, issued by a public certificate authority.
- A dedicated account with `read` access to the database.
- The `products` and `prices` tables from the public demo, imported with `--init-demo` before enabling protected access.

Follow the [protected server setup](../README.md#public-access) to create the reader and configure the certificate. TLS encrypts the connection and verifies that Neon is talking to your server. The normal username/password login uses SCRAM, which verifies the password without sending the password itself.

Neon needs access only to the PostgreSQL port. The native Quack endpoint stays on loopback. Allow the connection in your server's firewall and cloud network rules; opening a port alone does not create an account or grant write access.

## 2. Connect Neon to DoW

Open a Bash terminal in this repository. Use `psql` 16 or newer, which supports reading settings from the environment and using the system's trusted certificates.

There are two logins: **your login to Neon**, and **Neon's reader login to DoW**. The `PG*` settings below belong to Neon. Get its host, database and user from your Neon connection settings:

```bash
export PGHOST=your-neon-host PGDATABASE=neondb PGUSER=your-neon-user
export PGSSLMODE=verify-full PGSSLROOTCERT=system
unset PGPASSWORD
```

Now set the separate DoW endpoint. Use the actual hostname on its certificate and the reader account you created:

```bash
export DUCKONWIRE_FDW_HOST=db.example.com DUCKONWIRE_FDW_PORT=9496
export DUCKONWIRE_FDW_DATABASE=duckonwire DUCKONWIRE_FDW_USER=reader
read -r -s -p 'DoW reader password: ' DUCKONWIRE_FDW_PASSWORD
printf '\n'
export DUCKONWIRE_FDW_PASSWORD
psql -X -f examples/neon/setup.sql -f examples/neon/query.sql
unset DUCKONWIRE_FDW_PASSWORD
```

Enter your Neon password when `psql` asks. The earlier hidden prompt was for the **DoW reader password**. Neither password needs to appear in a command or connection URL.

Setup enables Neon's `postgres_fdw` extension and creates the `duckonwire` schema, two foreign tables and a connection for your current Neon user. It verifies the DoW certificate and hostname. Neon stores that reader login in its user mapping, so use the dedicated read-only account.

Setup runs in one transaction. If `duckonwire` already exists, it stops without replacing your existing configuration. You only need to run it once.

## 3. Query from your app

Your app keeps its normal Neon connection. Query the foreign tables like this:

```sql
SELECT id, name, brand
FROM duckonwire.products
ORDER BY id
LIMIT 5;
```

For products and their demo prices:

```sql
SELECT p.name, p.brand, r.price, r.day
FROM duckonwire.products p
JOIN duckonwire.prices r ON r.product_id = p.id
WHERE p.id <= 100
ORDER BY p.id;
```

[query.sql](../examples/neon/query.sql) also includes country totals and a latest-price query with a window function. The sample covers Argentina and Spain, with synthetic prices. You can join these foreign tables with your own Neon tables using the same SQL connection.

To use your own DoW tables, define foreign tables with matching column names and types. The example declares them explicitly; it does not require DuckDB to imitate PostgreSQL's full system catalogs.

## Transactions and permissions

Within a transaction, Neon retains its view of the remote DuckDB data. Writes committed by a direct DoW connection appear after the reading transaction ends and the next one starts. Protected readers support the cursor and savepoint operations used by `postgres_fdw`, including recovery from a failed read without replacing that snapshot.

The setup disables updates and truncation on the foreign server, and the DoW reader also rejects writes. Changing the Neon option does not grant the remote account write permission. Use a normal PostgreSQL driver connected **directly to DoW** for inserts, updates, deletes and write transactions. Write savepoints and atomic transactions spanning Neon and DoW are not supported by this integration.

## Remove the connection

When you no longer need it, run:

```sh
psql -X -f examples/neon/cleanup.sql
```

This removes the two foreign tables, your mapping, the schema and the foreign server from Neon. It leaves the data in DoW. Cleanup stops if you have added other dependent objects; it does not remove them with `CASCADE`.

## If it does not connect

Check that Neon can reach the hostname and port, the certificate covers that hostname, and the reader's username, password and database match the server rules. A certificate error should be fixed at the certificate or hostname; keep `verify-full` enabled.

For a local PostgreSQL test with a private certificate authority, set `DUCKONWIRE_FDW_SSLROOTCERT` to a CA file readable by the PostgreSQL **server**. A file on your laptop is not available to a hosted Neon server. Hosted Neon should use a publicly trusted certificate and the default system roots.

[Neon's guide](https://neon.com/docs/extensions/postgres_fdw) and the [PostgreSQL reference](https://www.postgresql.org/docs/current/postgres-fdw.html) explain the extension in more detail. The [test index](../tests/results/index.json) records what DoW has actually verified.

## Español

Tu aplicación sigue conectándose a Neon. Neon consulta las tablas de DoW cuando las necesita, sin copiarlas, y puede unirlas con sus tablas locales. Configura un servidor DoW accesible, su certificado y un usuario de solo lectura; después ejecuta `setup.sql` una vez y prueba `query.sql`. Hay dos contraseñas distintas: la tuya para entrar a Neon y la del lector con el que Neon entra a DoW. Para escribir, conecta tu aplicación directamente a DoW con una cuenta `write`; esa misma conexión también permite leer.
