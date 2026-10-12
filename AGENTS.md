# Working on DuckOnWire

DuckOnWire shares a DuckDB file through a C++ server. The server opens the file once; each client gets its own native connection and transaction. Start with [README.md](README.md), and use [tests/results/index.json](tests/results/index.json) to check what has actually been tested.

## Connections and login

| Mode | Client and authentication |
| --- | --- |
| Local PostgreSQL, `127.0.0.1:9496` | Stock PostgreSQL driver. `PGPASSWORD` is the server's shared `DUCKONWIRE_TOKEN`; `PGUSER` is a label, not an account or permission boundary. Local examples default to `PGSSLMODE=disable`. Use an encrypted SSH tunnel for remote access. |
| Protected PostgreSQL | Stock driver with real `PGUSER`, `PGPASSWORD` and the configured `PGDATABASE`. Requires TLS certificate/hostname verification (`PGSSLMODE=verify-full`, `PGSSLROOTCERT`) and SCRAM-SHA-256. Server options: `--access-file`, `--tls-cert`, `--tls-key`, optionally `--pg-host`. Rules grant database-wide `read` or `write` access. |
| Native Quack, `127.0.0.1:9495` | A DuckDB connection using `ATTACH` or `CONNECT`, the paired native engine and Quack protocol 7. HTTPFS/Quack are built in. Quack uses the local token and stays on loopback, including in protected mode. |

PostgreSQL clients use their usual driver and leave an existing DuckDB installation alone. Queries still use DuckDB SQL; full PostgreSQL catalogs, ORMs and network `COPY` are not supported. Check the version in each result before applying it to a different release.

`ATTACH 'quack:127.0.0.1:9495' AS remote (TOKEN '...')` makes remote tables available as `remote.main.table_name`, so a client can join them with local tables. `CONNECT remote` sends subsequent SQL and transactions to the server. Use that route for parameterized SQL and general CRUD: plain `ATTACH` does not transport bound parameters in the tested remote-read path, has incomplete UPDATE/DELETE fallback paths and can cache schema metadata. Do not turn an ATTACH read test into a claim of full CRUD compatibility. The [ATTACH example](examples/attach.cpp) uses synthetic rows and removes its own table when finished.

Quack is trusted local access: PostgreSQL's per-user read/write rules and query limits do not apply to that endpoint.

Set passwords through environment variables or protected input. Never put credentials in command arguments, URLs, logs, screenshots or result JSON. Without `DUCKONWIRE_TOKEN`, the owner generates a token in its private mode-0600 descriptor. Protected users have separate credentials; the descriptor token is not their PostgreSQL password. Never disable TLS verification to make a test pass.

The [Flask playground](examples/postgres/flask/app.py) is an ordinary Flask/psycopg application. Configure a genuine protected reader through backend `PG*` variables and verified TLS; system trust roots are the default, with `PGSSLROOTCERT` for private CAs. Keep connection settings out of browser inputs. Each worker uses a bounded connection pool initialized after forking. Its maintenance thread calls the driver's official `pool.check()` every 60 seconds, keeping idle sessions usable before the native endpoint's five-minute idle limit. Start that thread only after forking and stop it when the worker exits. Each request must cap result rows and query time, stop and join its cancellation timer, then roll back before returning the connection. The pool's background reset must complete before that session is available to another request. Close cancelled or broken connections so they are replaced. Validate actual write denial, connection reuse after more than five idle minutes, fresh snapshots, reset completion, cancellation recovery and worker shutdown when testing this example.

## Neon and postgres_fdw

The human setup guide is [docs/neon.md](docs/neon.md). Use [examples/neon/setup.sql](examples/neon/setup.sql), [query.sql](examples/neon/query.sql) and [cleanup.sql](examples/neon/cleanup.sql) through ordinary `psql`, not a custom client. Use psql/libpq 16+ for `\getenv` and system CA trust. The user's normal `PG*` settings connect to Neon; `duckonwire_host`, `duckonwire_port`, `duckonwire_database`, `duckonwire_user` and `duckonwire_password` are psql variables for the separate outgoing DuckOnWire connection. Setup can read them from `DUCKONWIRE_FDW_*` environment variables. Prompt for the remote password with hidden Bash `read -s`, export it briefly, and unset it after setup; do not put its value in `-v`, a URL, shell history, echoed SQL or public evidence.

The remote endpoint must be reachable from Neon, use a publicly trusted certificate and a real protected read-only account. Keep `sslmode=verify-full`, `updatable=false` and `truncatable=false`. The default `sslrootcert=system` uses trusted public CAs; `duckonwire_sslrootcert` / `DUCKONWIRE_FDW_SSLROOTCERT` can name a CA file readable by the PostgreSQL backend for an isolated fixture. Do not relax TLS verification. Map only `CURRENT_USER`, never `PUBLIC`. The mapping stores the reader credential in Neon; don't claim that credentials are absent from PostgreSQL catalogs. Manual foreign-table columns must match the demo schema; do not assume `IMPORT FOREIGN SCHEMA` or arbitrary PostgreSQL catalog compatibility.

Setup creates the `duckonwire` schema/server in one transaction and fails on existing names. Cleanup deliberately has no `CASCADE`: it must not remove other schemas, mappings or remote data. Keep the interactive window-query sample bounded; qualify changes with the actual provided SQL. PostgreSQL FDW transactions, cursor operations and savepoints require their own runtime checks; ordinary-driver tests do not establish full FDW or write support. Use an isolated Neon namespace/database for tests and preserve the user's permanent configuration.

## Ownership, isolation and transactions

- Concurrent clients connect to the owner; they do not open its DuckDB file directly. File aliases cannot bypass the lock. Use one connection per concurrent worker.
- Use an owned temporary server root for tests. Database, descriptor and server TLS/rules paths must remain within `--root`. Keep private keys/rules mode `0600` or `0400`. PostgreSQL fixtures may use `--pg-port 0`; Quack needs a positive reserved port.
- Stop only a process your task owns. Use graceful termination for cleanup; crash tests must target an isolated fixture owner. Preserve other servers, databases and user edits; do not overwrite an active library or remove another process's descriptor.
- Bind data values with the driver's prepared API. `COMMIT` confirms writes; `ROLLBACK` discards them. Disconnect rolls back pending work. After an explicit transaction fails, roll back before reuse.
- Native snapshots last until the transaction ends, including requested PostgreSQL `READ COMMITTED`. `SERIALIZABLE` is unsupported. On write conflicts, roll back and re-read. If a connection breaks during `COMMIT`, reconcile the outcome before repeating a non-idempotent write.
- PostgreSQL savepoints work only inside an explicit transaction on a protected read-only connection. Recoverable query errors can roll back to that savepoint while retaining the native snapshot. Writer and legacy shared-token connections reject savepoints with SQLSTATE `0A000`; internal, fatal or memory errors require a full rollback.
- Protected mode blocks external file/network SQL and configuration changes. Import data locally before enabling it. Limits and cancellation are cooperative engine controls, not hard process-memory or I/O deadlines.
- Protected buffers use three quarters of DuckDB's detected memory budget, capped at 1 GiB before services and configuration locking. This reserves headroom beyond the native default of 80% of available memory. Preserve container detection; verify small-memory containers with actual concurrent reads/writes, not only successful startup. The buffer limit does not bound total process RSS.

## Build and verify

Public build tooling is CMake/C++; ordinary Python/Lua client examples remain welcome. Do not introduce a custom SDK or Python build orchestration.

For the [Docker demo](README.md#try-with-docker), use `docker compose up --build -d` and `docker compose exec duckonwire psql -X -c 'SELECT 42'`. The image builds for its native host architecture; check the [Docker workflow](.github/workflows/docker.yml) before claiming AMD64/ARM64 runtime acceptance. The named volume has one native owner, private generated accounts and local verified TLS. `docker compose stop` preserves its database. Never log the volume's passwords or private keys, and remove only task-owned test volumes.

From the repository root, with CMake 3.24+, Ninja, C++17, Git and OpenSSL/libcurl development files:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
cmake -DSOURCE_DIR=. -DSERVER_BINARY=build/bin/duckonwire-server -P cmake/DocumentationTests.cmake
```

`tests/expected-checks.json` defines exact acceptance counts for CTest summaries, installed packages and releases. Update it when adding checks; keep count, named-result, source-head and definition-hash verification strict. CTest covers native sessions/transactions, PostgreSQL protocol, access rules, SCRAM, concurrent smoke and bootstrap regressions. The [CI workflow](.github/workflows/verify.yml) adds the two-million-row native benchmark and installed-package relocation checks. Run relevant checks after changes; do not repeat expensive workloads without a material reason or measure performance while another build/load is running.

The `native-attach` CTest runs the actual `attach-client` against its own temporary server. It checks remote reads, a join with local data, `CONNECT remote`, parameterized changes, transactions and cleanup. To run just this example's checks, build `attach-client` and `attach-tests`, then use `ctest --test-dir build -R native-attach --output-on-failure`. This executable is in current source builds; earlier packages may not contain it.

The [documentation workflow](.github/workflows/documentation.yml) checks guide links and actual server options, then runs native transactions, local/protected stock-driver login and concurrent client ingestion with complex reads. External S3/R2 credentials are not required by this workflow; consult the separately recorded cloud evidence.

Documentation and stock-driver example changes use that focused workflow. Engine/server/build changes trigger the full native workflow. If you change the full workflow's compiler or packaging steps, explicitly dispatch it before release; guide checks cannot qualify new build flags or a new engine.

Use [examples/postgres](examples/postgres) for stock-driver checks. Node `ingest.mjs` sends actual client values; `bulk.mjs` generates rows inside DuckDB. Their throughput scopes differ. Record row count, batch size, commits, reader overlap, integrity, hardware and timing exclusions; claim durable reopening only when it was tested.

## Pinned dependencies and evidence

Published native bundles were tested on macOS 26 ARM64 and Ubuntu 24.04 x86-64-v3 with glibc 2.39, libcurl 4 and OpenSSL 3. Do not promise older operating systems, emulated AVX2 on ARM, or Windows support from those results. Native Docker source builds separately test AMD64 and ARM64. Consult package manifests and the result index for current platform and binding qualification.

[cmake/Pins.cmake](cmake/Pins.cmake) is authoritative:

- DuckDB fork: `0cfb63a5f712fadf06a0de33217b7b03666ae66b`.
- Quack fork: `a6e7cb9243ee3d7ec0e5371b720be3292a32061f`.
- Official HTTPFS: `96a2f2e88e5dd075facbc5a65dc3afd67aa2bb44`.

The fork commits already contain the audited changes: never replay their patches. Official baseline revisions and patch hashes remain audit references. Keep `upstream/` clean; CMake creates independent working checkouts under `build/sources/`. Do not reset existing checkouts or silently advance pins; use a fresh build directory when source receipts differ.

Keep one README and organized JSON evidence under `tests/results/`; do not scatter Markdown reports. Distinguish fresh runtime acceptance, source/configuration checks and historical results. Include exact source/binary hashes and meaningful limitations; a successful configure is not a passed runtime test.

The approved demo contains manufacturer-brand products from Argentina/Spain and synthetic prices. Do not translate its real product/brand data when editing dummy examples. Use synthetic fixtures or authorized copies for stress tests; never write to private source databases. Public files, archives and embedded HTML/JSON must exclude private datasets, identifiers, SQL/results and credentials. Commit, push or publish only when the task authorizes it.

`--init-demo --demo-prices 1000000` generates repeatable 30-day price history in an empty database. Preserve product/price constraints and atomic initialization; a failed import must leave no partial tables. The default remains the CSV sample. Do not remove foreign keys or add indexes merely to improve a benchmark. Verify an actual plan and measure read gains, memory and confirmed write costs before adopting an index. Engine workers default to DuckDB's detected CPU capacity capped at two; `--threads 1..64` overrides it. Apply the cap before initialization, services and configuration locking. Verify the default under actual one-CPU and two-CPU quotas.

Use [latency.mjs](examples/postgres/node/latency.mjs) for complete stock-driver query round trips. Keep engine execution, native protocol, web processing and Internet latency separate. Compare identical data, query scope, resources, returned rows and result hashes; record warmup, pauses and median/95th percentile. Full-history aggregates must not be described as cheap point lookups. The web timer includes synchronous transaction cleanup; its pool still withholds a session until background reset finishes. Paging binds LIMIT/OFFSET, stays within existing result caps and uses stable SQL ordering; separate requests retain fresh snapshots, not one frozen multi-page view.
