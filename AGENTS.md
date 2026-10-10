# Working on DuckOnWire

DuckOnWire is a native C++ server: one `duckdb::DuckDB` owns one persistent file, with an independent native connection per client session. Read [README.md](README.md) for user setup and [tests/results/index.json](tests/results/index.json) for recorded qualifications.

## Connections and login

| Mode | Client and authentication |
| --- | --- |
| Local PostgreSQL, `127.0.0.1:9496` | Stock PostgreSQL driver. `PGPASSWORD` is the server's shared `DUCKONWIRE_TOKEN`; `PGUSER` is a label, not an account or permission boundary. Local examples default to `PGSSLMODE=disable`. Use an encrypted SSH tunnel for remote access. |
| Protected PostgreSQL | Stock driver with real `PGUSER`, `PGPASSWORD` and the configured `PGDATABASE`. Requires TLS certificate/hostname verification (`PGSSLMODE=verify-full`, `PGSSLROOTCERT`) and SCRAM-SHA-256. Server options: `--access-file`, `--tls-cert`, `--tls-key`, optionally `--pg-host`. Rules grant database-wide `read` or `write` access. |
| Native Quack, `127.0.0.1:9495` | Real DuckDB connection using `CONNECT`, the paired native engine and Quack protocol 7. HTTPFS/Quack are built in; no `LOAD` or custom connection wrapper. Quack uses the local token and stays on loopback, including in protected mode. |

PostgreSQL clients do not import, replace or change an existing client DuckDB installation. SQL still runs in DuckDB; do not promise full PostgreSQL SQL, catalogs, ORM or `COPY` compatibility. Published bundle capabilities and historical binding results must be checked separately from current source.

Quack is trusted local access: PostgreSQL's per-user read/write rules and query limits do not apply to that endpoint.

Set passwords through environment variables or protected input. Never put credentials in command arguments, URLs, logs, screenshots or result JSON. Without `DUCKONWIRE_TOKEN`, the owner generates a token in its private mode-0600 descriptor. Protected users have separate credentials; the descriptor token is not their PostgreSQL password. Never disable TLS verification to make a test pass.

## Ownership, isolation and transactions

- Concurrent clients connect to the owner; they do not open its DuckDB file directly. File aliases cannot bypass the lock. Use one connection per concurrent worker.
- Use an owned temporary server root for tests. Database, descriptor and server TLS/rules paths must remain within `--root`. Keep private keys/rules mode `0600` or `0400`. PostgreSQL fixtures may use `--pg-port 0`; Quack needs a positive reserved port.
- Stop only a process your task owns. Use graceful termination for cleanup; crash tests must target an isolated fixture owner. Preserve other servers, databases and user edits; do not overwrite an active library or remove another process's descriptor.
- Bind data values with the driver's prepared API. `COMMIT` confirms writes; `ROLLBACK` discards them. Disconnect rolls back pending work. After an explicit transaction fails, roll back before reuse.
- Native snapshots last until the transaction ends, including requested PostgreSQL `READ COMMITTED`. `SERIALIZABLE` is unsupported. On write conflicts, roll back and re-read. If a connection breaks during `COMMIT`, reconcile the outcome before repeating a non-idempotent write.
- Protected mode blocks external file/network SQL and configuration changes. Import data locally before enabling it. Limits and cancellation are cooperative engine controls, not hard process-memory or I/O deadlines.

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

CTest covers native sessions/transactions, PostgreSQL protocol, access rules, SCRAM, concurrent smoke and bootstrap regressions. The [CI workflow](.github/workflows/verify.yml) adds the two-million-row native benchmark and installed-package relocation checks. Run relevant checks after changes; do not repeat expensive workloads without a material reason or measure performance while another build/load is running.

The [documentation workflow](.github/workflows/documentation.yml) checks guide links and actual server options, then runs native transactions, local/protected stock-driver login and concurrent client ingestion with complex reads. External S3/R2 credentials are not required by this workflow; consult the separately recorded cloud evidence.

Documentation and stock-driver example changes use that focused workflow. Engine/server/build changes trigger the full native workflow. If you change the full workflow's compiler or packaging steps, explicitly dispatch it before release; guide checks cannot qualify new build flags or a new engine.

Use [examples/postgres](examples/postgres) for stock-driver checks. Node `ingest.mjs` sends actual client values; `bulk.mjs` generates rows inside DuckDB. Their throughput scopes differ. Record row count, batch size, commits, reader overlap, integrity, hardware and timing exclusions; claim durable reopening only when it was tested.

## Pinned dependencies and evidence

[cmake/Pins.cmake](cmake/Pins.cmake) is authoritative:

- DuckDB fork: `848126236a4a8879f232bed7ab5f702683a55300`.
- Quack fork: `a6e7cb9243ee3d7ec0e5371b720be3292a32061f`.
- Official HTTPFS: `96a2f2e88e5dd075facbc5a65dc3afd67aa2bb44`.

The fork commits already contain the audited changes: never replay their patches. Official baseline revisions and patch hashes remain audit references. Keep `upstream/` clean; CMake creates independent working checkouts under `build/sources/`. Do not reset existing checkouts or silently advance pins; use a fresh build directory when source receipts differ.

Keep one README and organized JSON evidence under `tests/results/`; do not scatter Markdown reports. Distinguish fresh runtime acceptance, source/configuration checks and historical results. Include exact source/binary hashes and meaningful limitations; a successful configure is not a passed runtime test.

The approved demo contains manufacturer-brand products from Argentina/Spain and synthetic prices. Do not translate its real product/brand data when editing dummy examples. Use synthetic fixtures or authorized copies for stress tests; never write to private source databases. Public files, archives and embedded HTML/JSON must exclude private datasets, identifiers, SQL/results and credentials. Commit, push or publish only when the task authorizes it.
