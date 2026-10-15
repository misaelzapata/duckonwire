# Accounts, database access and temporary blocks

Give each person or application its own login, then choose which databases it can use. A `read` account can query; a `write` account can query and change rows and tables. These permissions apply to both the PostgreSQL and native DuckDB connection options.

Use a current source build for native account permissions and IP protection. Earlier downloaded packages may not include them. This guide uses the [source build](../README.md#build-from-source), Bash, OpenSSL and an installed `psql`. From the repository folder, set:

```bash
dow_bin="$PWD/build/bin"
dow_root="$PWD/account-workspace"
umask 077
mkdir -p "$dow_root"
```

For an installed package containing these features, run from its folder and use `dow_bin="$PWD/bin"` instead. Keep this terminal for the server setup.

## 1. Create accounts for two databases

This example gives `app` read/write access to `shop`, `reader` read-only access to `shop`, and `app` read-only access to `reports`. The two `app` logins use different passwords. An account listed only under `shop` cannot log in to `reports`.

Choose three passwords in your password manager, each 32–1024 printable ASCII characters. Enter them at the hidden prompts; the password tool produces verifiers so the rules file does not store the original passwords:

```bash
read -r -s -p 'Shop app password: ' shop_password; printf '\n'
shop_verifier="$(printf '%s\n' "$shop_password" | "$dow_bin/duckonwire-password")"
read -r -s -p 'Shop reader password: ' reader_password; printf '\n'
reader_verifier="$(printf '%s\n' "$reader_password" | "$dow_bin/duckonwire-password")"
read -r -s -p 'Reports app password: ' reports_password; printf '\n'
reports_verifier="$(printf '%s\n' "$reports_password" | "$dow_bin/duckonwire-password")"
printf 'database\tshop\nuser\tapp\twrite\t%s\nuser\treader\tread\t%s\ndatabase\treports\nuser\tapp\tread\t%s\n' \
  "$shop_verifier" "$reader_verifier" "$reports_verifier" > "$dow_root/access.tsv"
chmod 600 "$dow_root/access.tsv"
unset shop_password reader_password reports_password shop_verifier reader_verifier reports_verifier
```

The `printf` writes real tabs. Each `database` row starts a section; the following `user` rows belong to it. Do not add blank lines or comments. For manual edits, use the [two-database example](../examples/access-multiple.tsv.example), replacing every placeholder with a generated verifier. Names use letters, digits, underscores, hyphens or dots, and must start with a letter, digit or underscore; usernames are case-sensitive.

Accounts are managed in this file, rather than through SQL `CREATE USER` or `GRANT`. Permissions cover the whole database, without table or row grants. Protected accounts cannot read external files or networks, load extensions or change server configuration. Import external data before enabling protected access.

## 2. Start with verified local TLS

Put the server certificate and private key inside `account-workspace`. For a first test on your own machine, create a short-lived certificate for `localhost`:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
  -keyout "$dow_root/server.key" -out "$dow_root/server.crt" \
  -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost'
chmod 600 "$dow_root/server.key"
```

This certificate is only for the local test. For direct remote PostgreSQL access, use a certificate for your real server hostname, issued by a CA your clients trust.

Stop any previous owner of these files, then start the server:

```bash
"$dow_bin/duckonwire-server" --root "$dow_root" \
  --database shop=data/shop.duckdb --database reports=data/reports.duckdb \
  --port 9495 --pg-port 9497 \
  --access-file access.tsv --tls-cert server.crt --tls-key server.key
```

Leave it running. This creates two databases if the files do not exist. PostgreSQL uses port `9497`, and clients choose `shop` or `reports` when logging in. Native Quack uses `127.0.0.1:9495` for `shop` and `127.0.0.1:9496` for `reports`. Its ports are consecutive in the order the databases were listed, so PostgreSQL must use a different port.

All clients connect to this server; they do not open its database files directly. Give each concurrent worker its own connection. Unfinished writes stay in that connection's transaction; `COMMIT` makes them visible to new transactions, and `ROLLBACK` or disconnect discards them. A reader keeps its snapshot until its transaction ends. Transactions do not commit two databases atomically.

## 3. Connect with PostgreSQL tools

Open another Bash terminal in the same repository folder. Use the certificate from the local setup, keeping hostname and certificate verification enabled:

```bash
export PGHOST=localhost PGPORT=9497 PGDATABASE=shop PGUSER=app
export PGSSLMODE=verify-full PGSSLROOTCERT="$PWD/account-workspace/server.crt"
read -r -s -p 'Shop app password: ' PGPASSWORD; printf '\n'
export PGPASSWORD
psql -X -v ON_ERROR_STOP=1 -c 'SELECT 42;'
```

This should return `42`. That connection can also create a table and read and change its rows. Use your usual PostgreSQL driver; SQL runs in DuckDB. [Client examples](../README.md#examples-and-tests) show bound values and transactions.

Try the reader account by entering its separate password:

```bash
export PGUSER=reader
read -r -s -p 'Shop reader password: ' PGPASSWORD; printf '\n'
export PGPASSWORD
psql -X -v ON_ERROR_STOP=1 -c 'SELECT 42;'
psql -X -v ON_ERROR_STOP=1 -c 'CREATE TABLE reader_must_not_create(id INTEGER);'
```

The query succeeds; creating the table must fail with a permission error. To use `reports`, set `PGDATABASE=reports PGUSER=app` and enter the **reports** app password. That login can read but cannot write. `reader` has no grant there, and the shop app password cannot authenticate the reports app login.

When finished, run `unset PGPASSWORD`. Keep passwords out of URLs, command arguments, logs and screenshots.

## 4. Connect through DuckDB

Use the paired native engine supplied by the build or package. The existing C++ examples use ordinary `duckdb::Connection`; there is no custom connection class. Protected Quack credentials use `username:password` in the existing `TOKEN` field. The server checks the verifier for that account and database; its shared local token cannot bypass account rules.

In a client Bash terminal in the repository folder, run the writer example against `shop`:

```bash
dow_bin="$PWD/build/bin"
export PGUSER=app
read -r -s -p 'Shop app password: ' PGPASSWORD; printf '\n'
export PGPASSWORD
DUCKONWIRE_ENDPOINT=quack:127.0.0.1:9495 \
  DUCKONWIRE_TOKEN="${PGUSER}:${PGPASSWORD}" "$dow_bin/cpp-crud-client"
unset PGPASSWORD
```

It creates or updates its demo rows and prints `"passed":true`. Use `dow_bin="$PWD/bin"` for an installed package. The [ATTACH example](../examples/attach.cpp) uses the same environment settings: `ATTACH` reads remote tables and joins them with local data; `CONNECT remote` sends subsequent queries, bound parameters, writes and transactions to the server. Reader accounts work with both routes for reading, but cannot run this writer example. Plain `ATTACH` has [limits for writes, parameters and cached schema metadata](../README.md#connect-for-parameters-writes-and-transactions).

Quack sends the account credential in its request. It does not perform PostgreSQL's SCRAM challenge exchange. The server's `--tls-cert` and `--tls-key` protect the PostgreSQL listener only. Keep Quack on loopback and encrypt remote traffic with an SSH tunnel or verified HTTPS termination.

For example, from your client machine open a tunnel to both native database ports:

```sh
ssh -N -L 19495:127.0.0.1:9495 -L 19496:127.0.0.1:9496 your-user@your-server
```

Leave the tunnel running. Use `DUCKONWIRE_ENDPOINT=quack:127.0.0.1:19495` for `shop`, or `quack:127.0.0.1:19496` for `reports`, with that database's account password. With HTTPS termination, native clients must verify the HTTPS server certificate, and the proxy forwards traffic to the loopback listener. The PostgreSQL TLS settings do not configure that proxy.

For direct remote PostgreSQL connections, restart with `--pg-host 0.0.0.0`, the real hostname's certificate and your network's allowed PostgreSQL port. Clients use that hostname, `PGSSLMODE=verify-full` and their trusted CA via `PGSSLROOTCERT`. Keep Quack on loopback.

## 5. Set connection and traffic limits

Protection starts with `--access-file` and covers every protected database and both protocols together:

| Default | What causes a temporary block |
| --- | --- |
| 5 failed credentials within 60 seconds | The fifth failure blocks the IP. A successful login does not erase previous failures. |
| 120 connections/logins per 60 seconds | The next attempt blocks the IP. PostgreSQL counts accepted TCP connections; Quack counts native login requests. |
| 6,000 protocol messages per 60 seconds | The next message blocks the IP. One query can use several messages. |
| 300-second block | Further blocked attempts do not extend the block. Counters reset when it expires. |

To change these limits, stop the server you started and restart it with the options you need. For example, this allows 10 failed credentials, 240 connections/logins and 12,000 messages, with a five-minute block:

```bash
"$dow_bin/duckonwire-server" --root "$dow_root" \
  --database shop=data/shop.duckdb --database reports=data/reports.duckdb \
  --port 9495 --pg-port 9497 \
  --access-file access.tsv --tls-cert server.crt --tls-key server.key \
  --auth-max-failures 10 --auth-window-seconds 60 --ban-seconds 300 \
  --max-connections-per-minute 240 --max-requests-per-minute 12000
```

A blocked IP cannot open a new session or keep sending messages through an existing one, even using a different account, database or protocol. A query already executing is not automatically cancelled. After expiry, a client can reconnect using the right credentials. Other IPs remain usable.

The server uses the socket's IP address and ignores forwarded-IP headers. People behind one NAT, proxy or SSH tunnel share its limits; many tunneled clients may all appear as loopback. Size limits for their combined traffic. Blocks live in memory and disappear on restart. Tracking holds at most 4,096 IPs; when full, new IPs are rejected until old entries expire, without evicting active blocks. These controls do not replace query timeouts or hard process-resource limits; see the [server options](../README.md#server-options).

## 6. Change or remove access

Edit the private `access.tsv` file under the correct database section:

| Change | Action |
| --- | --- |
| Add an account | Generate a verifier with `duckonwire-password`, then add a `user` row with `read` or `write`. |
| Change a password | Generate a new verifier and replace that account's verifier in each database you want to change. |
| Change permissions | Replace `read` with `write`, or `write` with `read`. |
| Revoke access | Remove that account's row from the database section. |

Keep at least one user in every database section, with no duplicate username in a section. Keep the file mode `0600` or `0400`. Restart the server to apply the changes: this closes old sessions and clients must log in under the new rules. Changing one database's account does not change its grants in another database. The Docker demo generates its own reader/writer rules on startup; these manual edits apply to a server you configure directly.

If a valid login stops working, check its database, username, password and whether that socket IP has reached a temporary block. Wait for the configured block to expire before reconnecting. Fix certificate or hostname errors while retaining TLS verification. If protected startup rejects a stored SQL definition, inspect the database locally with the server stopped and remove or rewrite the unsafe definition; the server checks persisted views, macros and table expressions before publishing the database.

## Español

Cada cuenta tiene permisos por base: `read` consulta y `write` también modifica. El archivo privado `access.tsv` configura ambas formas de conexión; reinicia el servidor para aplicar cambios o revocar acceso. PostgreSQL usa TLS verificado y SCRAM. Quack envía `usuario:contraseña` y necesita un túnel cifrado o HTTPS verificado al conectarse desde otra máquina. Los bloqueos temporales se comparten entre bases y protocolos; clientes detrás del mismo proxy o túnel también comparten sus límites.
