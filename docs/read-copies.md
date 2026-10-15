# Update a database copy while people keep reading

Use a read copy when another job prepares your data and applications only need to query it. DoW keeps the server and its connections running while you publish a newer copy. Work already in a transaction keeps its previous view; new transactions see the update.

This is an opt-in feature in the current source build. Ordinary `--database` connections still support reading and writing. A database published with `--read-copy` always stays read-only, including for accounts that normally have write permission.

## Try two small copies

From a [built source checkout](../README.md#build-from-source), prepare a new example workspace:

```sh
./build/bin/read-copy-example copy-workspace
```

It creates two tiny databases. The first has Coffee with stock 3; the second has stock 4. Each contains a `copy_version` table so an application can tell which copy its own transaction is reading. The example refuses an existing workspace.

Start the server in that terminal:

```sh
export DUCKONWIRE_TOKEN='copy-example-password-local'
./build/bin/duckonwire-server --root copy-workspace \
  --read-copy shop=copies/catalog.current
```

This local example uses one shared password on your own machine. For remote access, use the [account and encrypted-connection guide](access.md). The same read-only rule applies through PostgreSQL drivers and compatible native DuckDB connections.

In another terminal, open your installed `psql`:

```sh
export PGHOST=127.0.0.1 PGPORT=9496 PGDATABASE=shop PGUSER=reader PGSSLMODE=disable
export PGPASSWORD='copy-example-password-local'
psql -X -v ON_ERROR_STOP=1
```

At the SQL prompt, start a transaction and read both tables:

```sql
BEGIN;
SELECT version FROM copy_version;
SELECT name, stock FROM inventory ORDER BY id;
```

The results are version 1 and Coffee with stock 3. Leave this transaction open.

## Publish the second copy

In a third terminal, from the same checkout, replace the small request file:

```sh
dow_next="$(mktemp copy-workspace/copies/.catalog.XXXXXX)"
printf 'v2.duckdb\n' > "$dow_next"
mv "$dow_next" copy-workspace/copies/catalog.current
```

DoW checks that request regularly and validates the selected file before using it. Successful updates and rejected requests appear in the server's event output. Wait for the successful update event, then return to the SQL prompt:

```sql
SELECT version FROM copy_version;
SELECT name, stock FROM inventory ORDER BY id;
COMMIT;
SELECT version FROM copy_version;
SELECT name, stock FROM inventory ORDER BY id;
```

Before `COMMIT`, this transaction still returns version 1 and stock 3. Afterwards, the next queries return version 2 and stock 4. The server process, port and client connection stay the same. New clients also see version 2. Existing prepared queries are checked again against the updated copy.

Writes to this published copy fail. To change the data, prepare another complete file and publish it the same way.

## Use your own copies

The layout is deliberately small:

| Path | Purpose |
| --- | --- |
| `copies/catalog.current` | Your request: one filename and an optional final newline. |
| `copies/catalog.current.versions/` | Completed database files for this one published database. |
| `copies/catalog.current.active` | DoW's saved record of the last working copy. Let the server manage it. |

Keep these paths inside `--root`. The versions directory must belong to the server's operating-system user and have permission `0700`. Each completed version must be a regular file with permission `0400`, without symbolic links, hard links or a pending write-ahead log. Close and checkpoint the producing database before copying it here. Use a new filename for every new version; never overwrite a file the server may still be reading.

For example, after your data-producing job has closed `next.duckdb`:

```sh
cp next.duckdb copy-workspace/copies/catalog.current.versions/v3.duckdb
chmod 400 copy-workspace/copies/catalog.current.versions/v3.duckdb
dow_next="$(mktemp copy-workspace/copies/.catalog.XXXXXX)"
printf 'v3.duckdb\n' > "$dow_next"
mv "$dow_next" copy-workspace/copies/catalog.current
```

The request file must have permission `0600` or `0400`; `mktemp` creates a private file. It names only a file directly inside its versions directory, never a full path. Each database gets its own request file and versions directory.

DoW opens the candidate read-only and checks its saved database definitions before publishing it. This catches missing files, damaged headers/catalogs and unsafe saved queries. It does not scan every data block or prove your application's totals are correct: validate your produced data before publishing it.

## Keep related reads and cached results together

A page that runs several queries should use one connection and one transaction for those related reads. Read its version from a table inside the copy, such as `copy_version`, on that same transaction. Commit or roll back when the request finishes; close unfinished results before returning a connection to a pool.

If you cache results, include that version and the database name in the cache key. A reader finishing version 1 must not label its result as version 2 just because the server has published an update. The server's latest-update counter and the file's modification time do not identify an older transaction's view. Use a different version value inside every newly produced copy.

Separate page requests normally start fresh transactions. If all pages must show one frozen view, keep that transaction open until pagination finishes, with an application-defined time limit.

## Failed updates and restarts

A missing, damaged or rejected candidate leaves the last working copy available. DoW records the failure without printing passwords, row values or saved query text. After a failed request, replace the request file when you are ready to try again; unchanged failures do not flood the log every polling cycle.

After a restart, DoW first opens its saved working copy. It then checks a pending request normally. If that request is valid, it can publish it; if it is still bad, readers keep the working copy. If publication fails after replacement, DoW explicitly restores and verifies the previous copy rather than relying on SQL rollback. Readers that already started using a validated candidate can finish with it even if a later publication step fails and the server restores the previous copy. If restoration also fails, that copy becomes unavailable until a valid new request recovers it; other published databases keep serving.

Watch the update and failure events with your usual log monitoring. An unavailable copy requires attention; a rejected candidate should be fixed in the producing job. This feature does not send email or configure a monitoring service for you.

DoW leaves version files on disk. Old transactions, unfinished results and prepared plans can still hold an earlier file open, so do not delete old versions just because an update succeeded. Keep an appropriate retention policy in your producing job; automatic deletion is not included. A process tracks up to 4,096 distinct version paths; restart it deliberately before reaching that limit. The per-process update counter resets on restart and is for monitoring, not a cache key.

## Serve copies beside writable databases

You can publish a read copy and a normal database together:

```sh
./build/bin/duckonwire-server --root copy-workspace \
  --read-copy shop=copies/catalog.current --database jobs=data/jobs.duckdb \
  --pg-port 9497
```

PostgreSQL clients choose `shop` or `jobs` when connecting. `shop` stays read-only; `jobs` can read and write. Native ports are consecutive, so this two-database example moves PostgreSQL to port `9497`. Refreshing `shop` does not restart or replace `jobs`.

## Español

`--read-copy` permite actualizar una copia de lectura sin reiniciar el servidor. Prepara cada versión en un archivo nuevo, ciérralo y publica su nombre reemplazando el archivo de solicitud. Las transacciones en curso conservan su versión; las nuevas ven la actualización. Usa una sola transacción para las consultas relacionadas y añade la versión leída a las claves de caché. Una copia rechazada conserva la anterior; el servidor registra los cambios y guarda la última versión válida para reiniciarse.
