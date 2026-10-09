# DuckOnWire

Native DuckDB read/write access from multiple processes, using a C++ server and Quack `CONNECT`. One server owns the persistent DuckDB file. Clients use real DuckDB connections, prepared statements and transactions against that same database.

HTTPFS and Quack are built into the paired engine. A normal connection can execute `CONNECT` without `LOAD`, extension installation or unsigned-extension configuration. The server and C/C++ clients run as native programs.

The project is an experimental **0.2.0-rc.4 candidate** and pins a development version of DuckDB. Own code is [MIT licensed](LICENSE); dependencies retain their licenses in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

The [DuckOnWire repository](https://github.com/misaelzapata/duckonwire) contains the server, examples, tests and CI. Engine changes live in the [DuckDB fork](https://github.com/misaelzapata/duckdb/tree/duckonwire/native) and [Quack fork](https://github.com/misaelzapata/duckdb-quack/tree/duckonwire/native), on the `duckonwire/native` branches. CMake selects fixed commits from those forks; their changes are already committed, so it does not apply the core/Quack patches again.

The [test results index](tests/results/index.json) distinguishes the current fork pins from the recorded **rc.3 upstream-plus-patch build**. The measurements and native acceptance below belong to that recorded build. Fork source content was checked against the qualified source. For binary acceptance of a fork build, check its commit and platform in [GitHub Actions](https://github.com/misaelzapata/duckonwire/actions) and the package manifest/checksums on [GitHub Releases](https://github.com/misaelzapata/duckonwire/releases). That evidence is produced after compilation and is reported separately from the source snapshot.

The native benchmark inserted **2,000,000 rows per mode with four writer connections and four reader connections**, then verified row counts and three checksums after reopening the database. All four readers completed queries while writes were active. The six runs cover [macOS locally](tests/results/benchmark.json), [Germany](tests/results/benchmark-germany.json) and [AWS São Paulo](tests/results/benchmark-aws.json): **12 million verified rows across separate databases**.

| Measured mode | Insert rows/s | Read mean (ms) | Read p50 (ms) | Read p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Local macOS — embedded | 2,765,342 | 19.43 | 14.98 | 47.92 |
| Local macOS — Quack loopback | 1,773,511 | 26.67 | 24.82 | 56.92 |
| Germany Linux — embedded | 616,371 | 54.52 | 18.46 | 180.80 |
| Germany Linux — Quack loopback | 482,686 | 86.55 | 94.40 | 200.04 |
| AWS São Paulo Linux — embedded | 283,705 | 200.40 | 48.67 | 571.24 |
| AWS São Paulo Linux — Quack loopback | 181,259 | 244.73 | 239.42 | 493.65 |

The local host is Apple M1 Pro with 8 logical CPUs and 16 GiB RAM. Germany used an Ubuntu 24.04 Docker container capped at **2 CPU cores and 4 GiB RAM** on an AMD EPYC host with 6 visible CPUs and approximately 12 GB RAM. AWS used the same Linux native build in a container capped at **2 CPU cores and 768 MiB RAM** on an Intel Xeon host with 2 visible CPUs and approximately 2 GB RAM. Every run used two DuckDB engine threads. These are recorded workloads under different hardware and memory limits, rather than a controlled comparison between hosts.

Each mode runs one native benchmark client process with eight worker threads and eight independent DuckDB connections; Quack also runs a separate native server process. Each run committed 80 batches of 25,000 rows using typed prepared `INSERT SELECT range` statements. DuckDB generates the rows inside each batch; the client sends bound range parameters. These are batch inserts, not two million individual network round trips. All Quack measurements use loopback within the measured host, and do not measure WAN latency.

## Build and test

Run these commands from the repository root. Requirements: CMake 3.24+, Ninja, Git, a C++17 compiler, OpenSSL and libcurl development files. DuckDB's upstream build may also use Python 3; this repository's server, tests and benchmark are C++.

```sh
git clone https://github.com/misaelzapata/duckonwire.git
cd duckonwire
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

CMake fetches the pinned DuckDB/Quack forks and official HTTPFS source into `upstream/`, keeps those checkouts clean, and prepares independent source copies in `build/sources/`. The fork commits already contain the core/Quack changes. Both directories are generated and ignored by Git. The shared engine is built under `build/lib/`; programs are built under `build/bin/`.

The server uses POSIX facilities. Native execution has been verified on macOS ARM64 and Ubuntu 24.04 Linux x86-64-v3 with glibc 2.39. Windows support is not established.

An extracted prebuilt native archive uses `bin/` and `lib/`. From its root, start with `bin/duckonwire-server --root . --init-demo` after setting the token, and run `bin/cpp-crud-client` or `bin/c-crud-client` in another terminal. To measure that archive, use `bin/duckonwire-benchmark --mode both --rows 2000000 --writers 4 --readers 4 --batch 25000 --output benchmark.json`. The source archive uses the CMake commands above. Native archives contain the C/C++ programs and engine; other language bindings require separate pairing.

Published downloads are listed on [GitHub Releases](https://github.com/misaelzapata/duckonwire/releases). GitHub provides source archives for the release tag; the rc.4 prerelease workflow builds native C/C++ bundles for macOS ARM64 and Linux x86-64-v3/Ubuntu 24.04, including the committed project sources. Check the matching Actions run and release manifests for the exact build's acceptance and publication status. The other eight driver binaries are not included in those bundles.

To package a completed native build, run from a clean, committed Git checkout with `BUILD_TESTING=ON`:

```sh
cmake -DSOURCE_DIR="$PWD" -DBUILD_DIR="$PWD/build" \
  -DVERSION=0.2.0-rc.4 -DPLATFORM=macos26-arm64 \
  -DOUTPUT_DIR="$PWD/build/packages" -P cmake/Package.cmake
```

For a Linux x86-64-v3 build, use `-DPLATFORM=linux-x86_64-v3`; this labels the actual build and does not cross-compile it. [Package.cmake](cmake/Package.cmake) installs the paired engine, headers and native programs, includes the committed project sources, then runs installed native acceptance and a 10,000-row smoke test in both modes. It writes `duckonwire-0.2.0-rc.4-PLATFORM.tar.gz`, a matching `.manifest.json` and `.sha256` under `build/packages/`, and preserves existing outputs.

## Run the server and ordinary CRUD

On the first start, choose a token and load the public demonstration catalog:

```sh
export DUCKONWIRE_TOKEN='replace-with-your-own-token'
build/bin/duckonwire-server --root . --init-demo
```

The native initializer loads the included CSVs in one transaction and preserves their foreign key and country constraints. `--init-demo` accepts only an empty database; it refuses to change an existing nonempty database. On subsequent starts, omit that flag:

```sh
build/bin/duckonwire-server --root .
```

By default, the server listens at `quack:127.0.0.1:9495` and opens `data/demo.duckdb` under its root. Without `--init-demo`, it creates an empty database if needed. Only the server opens that file for writing. It creates a private `.runtime/native.json` descriptor and removes it on graceful shutdown. If no token is configured, it generates one. Stop with Ctrl-C or SIGTERM.

In a second terminal, use the same token:

```sh
export DUCKONWIRE_TOKEN='replace-with-your-own-token'
build/bin/cpp-crud-client
build/bin/c-crud-client
```

`DUCKONWIRE_ENDPOINT` is optional and defaults to `quack:127.0.0.1:9495`. Clients take connection settings from these environment variables, without database paths or private descriptor arguments.

The [C++ example](examples/crud_cpp.cpp) and [C example](examples/crud_c.c) create `demo_products`, bind Cafe and Arroz values, update stock, select both rows, delete Arroz and commit. Each example leaves its own Cafe row with price `19.99` and stock `4`, and can run again without changing another client's rows. Successful output includes:

```json
{"passed":true,"language":"C++","rows":[[1,"Cafe","19.99",4]]}
```

The connection is an ordinary native DuckDB object. For example, the connection statement uses DuckDB's SQL grammar:

```cpp
duckdb::DuckDB engine(nullptr);
duckdb::Connection connection(engine);
auto result = connection.Query(
    "CONNECT 'quack:127.0.0.1:9495' (TOKEN 'your-token')");
if (result->HasError()) result->ThrowError();
```

The runnable examples read and SQL-escape the endpoint/token from the environment, then bind data values as native DuckDB values. Applications can link the CMake target `DuckDB::DuckDB` and use the standard C or C++ API.

## Reads and transactions

Each connection that executes `CONNECT` gets an independent server session. Use separate connections for concurrent work. Without `BEGIN`, each statement uses autocommit. `BEGIN` groups statements; `COMMIT` confirms changes and `ROLLBACK` discards pending changes.

A transaction that reads stock `3` retains its snapshot while another connection changes and commits stock `4`. The reader still sees `3` until it ends its transaction; its next transaction sees `4`.

```mermaid
sequenceDiagram
    participant R as Reader connection
    participant S as C++ server / DuckDB file owner
    participant W as Writer connection
    R->>S: BEGIN; SELECT stock
    S-->>R: 3
    W->>S: BEGIN; UPDATE stock = stock + 1
    R->>S: SELECT stock
    S-->>R: 3, writer still pending
    W->>S: COMMIT
    S-->>W: Committed
    R->>S: SELECT stock
    S-->>R: 3, retained snapshot
    R->>S: COMMIT; BEGIN; SELECT stock
    S-->>R: 4, new snapshot
    R->>S: COMMIT
```

With the server running and the token configured, execute the [C++ transaction flow](examples/transaction_flow.cpp):

```sh
build/bin/transaction-flow
```

It checks independent sessions, typed CRUD, reads `3 → 3 → 3 → 4`, rollback of a stock change to `99`, reader progress during a pending write, and a conflict when two writers update the same row. Its final committed row is `[[1,"Cafe","19.99",4]]`. It removes only the temporary table it created; an existing `transaction_flow_products` table makes it fail without changing that table.

After a write conflict, roll back and decide whether to repeat the complete transaction using freshly read data. A connection failure during `COMMIT` can leave the outcome uncertain: the change may be persisted even though the response was lost. Reconcile the operation before retrying an increment or another non-idempotent write. The transport does not automatically retry writes.

Closing a connection ends its session and rolls back pending work. In the paired Python binding, `cursor()` creates an independent connection/session. Closing a driver's result or statement frees that result; the transaction belongs to its connection. A long-running read transaction retains its snapshot, and query latency still depends on CPU, I/O and the query.

## Native benchmark

Run the C++ benchmark against the embedded engine and the Quack server:

```sh
build/bin/duckonwire-benchmark --mode both \
  --rows 2000000 --writers 4 --readers 4 --batch 25000 \
  --output build/results/benchmark.json
```

The C++ harness uses its own generated database and server, with 20,000 generated catalog products and 2,000,000 price rows. Four writer threads and four reader threads each own an independent native `Connection` in one client process. The embedded case shares that process's database; the Quack case connects those sessions to a separate server process over loopback. Writers perform typed prepared `INSERT SELECT range` statements in explicit transactions. The CTest smoke case uses a smaller dataset.

Each recorded environment contains one completed run per mode on separate fresh databases, without concurrent compilation. Insert throughput includes commits and excludes setup; OS caches are not flushed. Aggregate read latency combines the actual completed samples from point joins, count/checksum, whole-table join/GROUP BY and latest-price window queries for 256 catalog products. Its mean is weighted by sample count, and p50/p95 use nearest rank over the combined samples. The JSON retains each query's separate timings, sample counts, reader progress, hardware/container limits and durability checks. Local embedded completed 151 read queries and Quack completed 170; Germany completed 239 and 193; AWS completed 141 and 184. Their query mixtures need not be identical, so these figures describe the recorded workload rather than an equal-query microbenchmark. The cloud tests ran inside each host over loopback, with the client and Quack owner sharing the container's resource limits.

## Public demonstration catalog

The included CSV catalog has **46,462 real products** from **178 manufacturer brands**: **29,274 in Argentina** and **17,188 in Spain**. Country identifies the product's market, not its manufacturing origin. It excludes retailer/private-label brands, stores, URLs and private identifiers. Missing basic fields remain empty. Prices are reproducible synthetic values.

See [schema.sql](examples/demo/schema.sql), [load.sql](examples/demo/load.sql), [products.csv](examples/demo/products.csv), [prices.csv](examples/demo/prices.csv) and the [public queries](examples/demo/queries.sql). The schema preserves the product/price foreign key. The server validates constraints for writes through `CONNECT`.

## Other language examples

The repository contains examples for ten languages using their ordinary DuckDB APIs. The default CMake build produces the server, C/C++ clients, tests and benchmark. The other official drivers must be built and paired with this patched native engine, including binding changes where required; CMake does not build those eight bindings. Stock packages that bundle a different DuckDB engine do not provide this server's Quack protocol.

| Language | CRUD example |
| --- | --- |
| Python | [crud.py](examples/python/crud.py) |
| Node.js | [crud.mjs](examples/node/crud.mjs) |
| Go | [main.go](examples/go/crud/main.go) |
| Rust | [crud.rs](examples/rust/src/crud.rs) |
| Java | [NormalCrud.java](examples/java/NormalCrud.java) |
| Swift | [Crud.swift](examples/swift/Crud.swift) |
| C# | [Program.cs](examples/csharp/crud/Program.cs) |
| Lua | [client.lua](examples/lua/client.lua) |

Java uses the official JDBC source revision `5b624bb72a883e73356ed9f3a5109e1886383636` with the included [JNI compatibility patch](patches/duckdb-java.patch). It preserves the public Java API while making duplicated connections inherit `CONNECT`, transaction configuration use the connected native session, and connection close end that session. Node requires its official native binding to use the paired `v2.1.0-dev0` engine; `DUCKONWIRE_NODE_RUNTIME` can select a directory containing its `node_modules`. Lua can use `DUCKONWIRE_LIBRARY` or find the shared library under `lib/` or `build/lib/`.

Bindings retain their own limitations. The tested Rust driver reports `is_autocommit()` as true and preserves error text rather than native exception subclasses; transaction effects must be checked through data. Rust LIST/STRUCT parameters and Go DECIMAL parameters have driver-specific limitations. Remote UDFs, DataFrame registration and every relational method are not established features.

## Source layout and compatibility

```text
.
├── CMakeLists.txt
├── cmake/          Pinned bootstrap and extension configuration
├── src/            Native server
├── include/        Own native headers
├── tests/          Native acceptance and concurrent benchmark
├── examples/       CRUD clients, transaction flow and public catalog
├── patches/        Canonical upstream diffs and binding patch references
├── upstream/       Generated clean fork/official Git checkouts; ignored
└── build/          Generated source copies, library and programs; ignored
```

Build source pins and upstream provenance are in [cmake/Pins.cmake](cmake/Pins.cmake):

| Component | Build repository | Fixed commit |
| --- | --- | --- |
| DuckDB | [misaelzapata/duckdb](https://github.com/misaelzapata/duckdb) (`duckonwire/native`) | `848126236a4a8879f232bed7ab5f702683a55300` |
| Quack | [misaelzapata/duckdb-quack](https://github.com/misaelzapata/duckdb-quack) (`duckonwire/native`) | `a6e7cb9243ee3d7ec0e5371b720be3292a32061f` |
| HTTPFS | [duckdb/duckdb-httpfs](https://github.com/duckdb/duckdb-httpfs) | `96a2f2e88e5dd075facbc5a65dc3afd67aa2bb44` |

The fork changes are based on official DuckDB `638b09261a334feb2f7deb742fe3996db3c974b1` and Quack `984d45d27e4042906328fb2bd0cdb631d0e602a7`. [duckdb.patch](patches/duckdb.patch) and [quack.patch](patches/quack.patch) retain the canonical diffs for review and historical reproduction; CMake does not replay them on the fork commits. Binding patches remain separate from the engine forks.

The paired engine reports `v2.1.0-dev0` and uses **Quack protocol 7**. Client/server version mismatches are rejected before executing SQL. Several independent processes must use `CONNECT` to this server; opening the persistent file independently from those processes still follows DuckDB's file-lock rules.

To incorporate future official changes, use a new integration branch in each fork. For DuckDB, the preparation is:

```sh
git clone --branch duckonwire/native https://github.com/misaelzapata/duckdb.git duckdb-work
cd duckdb-work
git remote add upstream https://github.com/duckdb/duckdb.git
git fetch upstream
git checkout -b duckonwire/sync-YYYYMMDD duckonwire/native
git merge --no-ff REVIEWED_UPSTREAM_COMMIT
```

Replace `REVIEWED_UPSTREAM_COMMIT` with a reviewed fixed hash; use the corresponding repositories for Quack. Merge or rebase on that disposable integration branch, resolve conflicts, and select its exact candidate commit in DuckOnWire. Run a fresh CMake build, CTest, both benchmark modes and the affected paired-driver checks before integrating or releasing it. Update the pins and results index with actual evidence. Keep published pinned commits reproducible; never build by following a moving branch HEAD.

## Español

DuckOnWire permite leer y escribir desde varios procesos mediante conexiones DuckDB nativas y `CONNECT`. Un único servidor C++ abre el archivo; cada cliente tiene una sesión y transacción independientes. HTTPFS y Quack vienen integrados, sin `LOAD` ni configuración de extensiones sin firmar.

El repositorio `misaelzapata/duckonwire` contiene nuestro servidor, ejemplos, pruebas y CI. Los cambios del motor están en los forks `misaelzapata/duckdb` y `misaelzapata/duckdb-quack`, rama `duckonwire/native`, con commits fijos. CMake no vuelve a aplicar sus parches. Los benchmarks y pruebas registrados corresponden a rc.3, con upstream más parches. La aceptación binaria de cada build desde forks se consulta por commit y plataforma en [GitHub Actions](https://github.com/misaelzapata/duckonwire/actions) y en los manifiestos/checksums del release, generados después de compilar. Para actualizar upstream, trabajar en una rama nueva, integrar un commit revisado y volver a probar antes de cambiar los pins; nunca seguir un HEAD móvil.

Desde la raíz del repositorio:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
export DUCKONWIRE_TOKEN='replace-with-your-own-token'
build/bin/duckonwire-server --root . --init-demo
```

`--init-demo` carga el catálogo una sola vez en una base vacía y rechaza una base con tablas existentes. Para reiniciar, usar `build/bin/duckonwire-server --root .` sin ese flag. En otra terminal, configurar el mismo token y ejecutar `build/bin/cpp-crud-client`, `build/bin/c-crud-client` o `build/bin/transaction-flow`. El endpoint predeterminado es `quack:127.0.0.1:9495`; puede cambiarse con `DUCKONWIRE_ENDPOINT`. Los ejemplos CRUD dejan Cafe con precio `19.99` y stock `4`, y se pueden repetir.

Sin `BEGIN`, cada sentencia usa autocommit. `BEGIN` conserva un snapshot de lectura: el lector ve stock `3` incluso después de que el escritor confirme `4`; al terminar e iniciar otra transacción ve `4`. `ROLLBACK` descarta escrituras pendientes. Un conflicto sobre la misma fila exige rollback y decidir si repetir la transacción completa. Si se corta la conexión durante `COMMIT`, comprobar el resultado antes de repetir la escritura.

El [benchmark C++ local](tests/results/benchmark.json), [Alemania](tests/results/benchmark-germany.json) y [AWS São Paulo](tests/results/benchmark-aws.json) insertaron dos millones de filas por modo con cuatro conexiones escritoras y cuatro lectoras, en 80 transacciones de 25.000 filas: doce millones de filas verificadas en seis bases separadas. El cliente es un proceso nativo con ocho threads y conexiones independientes; Quack añade un proceso servidor. En el Apple M1 Pro: embebido **2.765.342 filas/s** y Quack **1.773.511 filas/s**. En Alemania, con límite Docker de **2 CPU y 4 GiB**: **616.371** y **482.686 filas/s**. En AWS, con **2 CPU y 768 MiB**: **283.705** y **181.259 filas/s**. La tabla inicial muestra media, p50 y p95 de lectura. Las diferencias de hardware y memoria impiden comparar los hosts en igualdad de condiciones.

Son `INSERT SELECT range` por lotes: DuckDB genera las filas y el cliente envía parámetros de rango, no dos millones de viajes de red. Cada modo usa dos threads del motor. Setup excluido, caché del sistema sin limpiar, sin compilaciones simultáneas y una medición registrada por modo y entorno. Los cuatro lectores progresaron durante las escrituras y los conteos/checksums sobrevivieron al cierre y reapertura. Quack se midió por loopback en cada host; estos datos no incluyen latencia WAN.

El catálogo público contiene 46.462 productos reales de Argentina y España, sin marcas blancas ni tiendas; sus precios son ficticios. CMake compila el servidor, C/C++, pruebas y benchmark. Los demás ejemplos necesitan bindings oficiales emparejados. Código propio bajo MIT; esta es la única guía del candidato rc.4.

En un archivo nativo precompilado, usar `bin/` en lugar de `build/bin/`: `bin/duckonwire-server --root . --init-demo` y `bin/cpp-crud-client`. El archivo fuente se compila con los comandos CMake anteriores. Las descargas se publican en [GitHub Releases](https://github.com/misaelzapata/duckonwire/releases); consultar la ejecución y los manifiestos correspondientes para comprobar su estado. Sus paquetes nativos incluyen C/C++, no los otros ocho bindings.

El comando `cmake -P cmake/Package.cmake` mostrado arriba requiere un checkout limpio y confirmado, más el build completo con pruebas. Verifica los binarios instalados y genera el archivo, manifiesto y checksums en `build/packages/`. Usar la etiqueta `linux-x86_64-v3` para un build Linux y `macos26-arm64` para macOS; la etiqueta no cambia la arquitectura compilada.

## 中文简述

DuckOnWire 是原生 C++ 项目，使用 DuckDB 和内置的 Quack、HTTPFS。只有服务器进程打开持久数据库文件；客户端通过真实的 DuckDB API 和 `CONNECT` 读写同一份数据，无需 `LOAD`。

服务器、示例、测试和 CI 位于 `misaelzapata/duckonwire`；引擎修改位于 DuckDB 和 Quack 的 forks，分支为 `duckonwire/native`。CMake 固定到明确的提交，不重复应用核心补丁。现有测量来自此前 rc.3 的 upstream 加补丁构建。各 fork 构建的二进制验收结果按提交和平台记录在 [GitHub Actions](https://github.com/misaelzapata/duckonwire/actions) 及编译后生成的发布清单和校验文件中，请查看对应记录。更新 upstream 时先在新分支集成经过审查的提交，重新测试后再更新固定版本，不跟随移动的 HEAD。下载见 [GitHub Releases](https://github.com/misaelzapata/duckonwire/releases)：源代码包以及 macOS/Linux 原生 C/C++ 包；其余八种语言的 bindings 需另行配对。

在仓库根目录运行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
export DUCKONWIRE_TOKEN='replace-with-your-own-token'
build/bin/duckonwire-server --root . --init-demo
```

`--init-demo` 只接受空数据库，首次启动时载入公开商品目录。重新启动时使用 `build/bin/duckonwire-server --root .`，不加此参数。在另一个终端设置相同的 token，然后运行 `build/bin/cpp-crud-client` 或 `build/bin/transaction-flow`。默认地址为 `quack:127.0.0.1:9495`。`BEGIN` 保留读取快照，`COMMIT` 确认写入，`ROLLBACK` 撤销未提交的修改。同一行的并发写入可能发生冲突；`COMMIT` 期间断线时，应先确认结果再重试。公开示例包含阿根廷和西班牙的真实商品以及合成价格。项目采用 MIT 许可证，详细构建说明和限制见上文。

每种模式实测写入 200 万行：一个原生客户端进程内有八个工作线程和独立连接，四个写入、四个读取；Quack 另有一个服务器进程。本机嵌入式约 276.5 万行/秒，Quack 回环连接约 177.4 万行/秒。德国 Linux 容器限制为 2 CPU、4 GiB，分别约 61.6 万和 48.3 万行/秒；AWS 为 2 CPU、768 MiB，分别约 28.4 万和 18.1 万行/秒。三种环境共六次测量，验证了 1,200 万行；硬件和内存限制不同，不能视为等条件的主机比较。读取均值、p50 和 p95 见上表。写入采用每批 25,000 行的 `INSERT SELECT range`，并非 200 万次网络往返。四个读取连接均在写入期间完成查询，重新打开数据库后行数和校验值一致。所有 Quack 数据均为同一主机内的回环测量，不代表跨网络延迟。预编译包使用 `bin/` 而非 `build/bin/`，源代码包按上面的 CMake 命令构建。
