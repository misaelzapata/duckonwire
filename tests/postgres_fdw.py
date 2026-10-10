"""Exercise a real postgres_fdw backend and a protected DuckOnWire reader.

All servers, certificates, passwords and data belong to this test fixture.
No project .env file or existing database is opened.
"""

import argparse
import contextlib
import datetime
import decimal
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile
import threading
import time

import psycopg


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--password-program", type=Path, required=True)
    parser.add_argument("--pg-bin", type=Path, default=Path(shutil.which("initdb") or ".").parent)
    parser.add_argument("--psql", type=Path, default=Path(shutil.which("psql") or "psql"))
    parser.add_argument("--examples", type=Path, default=Path(__file__).resolve().parents[1] / "examples/neon")
    parser.add_argument("--fixture-parent", type=Path, default=Path("build/test-results"))
    parser.add_argument("--rows", type=int, default=16003)
    args = parser.parse_args()
    args.server = args.server.resolve()
    args.password_program = args.password_program.resolve()
    args.pg_bin = args.pg_bin.resolve()
    args.psql = args.psql.resolve()
    args.examples = args.examples.resolve()
    if args.rows < 1003:
        parser.error("--rows must be at least 1003 to exercise multiple remote pages")
    args.fixture_parent.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix="fdw-fixture-", dir=args.fixture_parent.resolve()))
    fixture.chmod(0o700)
    owner = fixture / "owner"
    private = owner / "private"
    private.mkdir(parents=True, mode=0o700)
    pgdata = fixture / "postgres"
    native_process = None
    pg_started = False
    native_log = None
    credentials = [secrets.token_hex(32) for _ in range(3)]
    token, writer_password, reader_password = credentials
    environment = dict(os.environ)
    for key in list(environment):
        if key.startswith("PG") or key.startswith("DUCKONWIRE_"):
            environment.pop(key)
    report = {
        "passed": False,
        "scope": "Owned local PostgreSQL postgres_fdw backend, real protected reader, actual public SQL tutorial",
        "native_used": True,
        "remote_neon_accessed": False,
        "existing_databases_opened": False,
        "tls": "verify-full with an owned CA visible to the owned PostgreSQL backend",
        "server_sha256": digest(args.server),
        "tutorial_sha256": {name: digest(args.examples / name) for name in ["setup.sql", "query.sql", "cleanup.sql"]},
        "checks": [],
    }

    def require(condition, message):
        if not condition:
            raise AssertionError(message)

    def run(command, *, input_text=None, env=None, timeout=45):
        result = subprocess.run([str(x) for x in command], input=input_text, env=env or environment,
                                capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            # Certificates, mappings and credentials never enter error output.
            raise RuntimeError("Fixture command failed: " + Path(str(command[0])).name)
        return result.stdout.strip()

    def expect_error(connection, statement, state, *, prepare=None):
        try:
            connection.execute(statement, prepare=prepare)
        except psycopg.Error as error:
            require(error.sqlstate == state, "Unexpected SQLSTATE: " + str(error.sqlstate))
            return
        raise AssertionError("Statement unexpectedly succeeded")

    def check(name, callback):
        try:
            details = callback()
            item = {"name": name, "passed": True}
            if isinstance(details, dict):
                item.update(details)
            report["checks"].append(item)
        except Exception as error:
            message = str(error)
            for secret in credentials:
                message = message.replace(secret, "[redacted]")
            item = {"name": name, "passed": False, "error": message[:500]}
            if isinstance(error, psycopg.Error):
                item["sqlstate"] = error.sqlstate
            report["checks"].append(item)
            raise

    try:
        openssl = shutil.which("openssl")
        require(bool(openssl), "openssl is required")
        run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
             "-subj", "/CN=DuckOnWire owned FDW CA", "-addext", "basicConstraints=critical,CA:TRUE",
             "-keyout", private / "ca-key.pem", "-out", private / "ca.pem"])
        run([openssl, "req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256", "-subj", "/CN=localhost",
             "-keyout", private / "key.pem", "-out", private / "request.pem"])
        (private / "extensions").write_text(
            "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n")
        run([openssl, "x509", "-req", "-sha256", "-days", "2", "-in", private / "request.pem",
             "-CA", private / "ca.pem", "-CAkey", private / "ca-key.pem", "-CAcreateserial",
             "-extfile", private / "extensions", "-out", private / "cert.pem"])
        writer_verifier = run([args.password_program], input_text=writer_password + "\n")
        reader_verifier = run([args.password_program], input_text=reader_password + "\n")
        (private / "access.tsv").write_text(
            "database\tduckonwire\nuser\twriter\twrite\t" + writer_verifier +
            "\nuser\treader\tread\t" + reader_verifier + "\n")
        for path in private.iterdir():
            path.chmod(0o600)

        run([args.pg_bin / "initdb", "-D", pgdata, "-A", "trust", "--no-locale", "--encoding=UTF8"])
        pgport = available_port()
        with (pgdata / "postgresql.conf").open("a") as settings:
            settings.write("\nlisten_addresses='127.0.0.1'\nport=" + str(pgport) +
                           "\nunix_socket_directories=''\nlog_statement='none'\nlog_min_error_statement='panic'\n")
        run([args.pg_bin / "pg_ctl", "-D", pgdata, "-l", fixture / "postgres.log", "-w", "start"])
        pg_started = True
        pg_user = run([args.pg_bin / "initdb", "--version"])
        report["postgresql"] = pg_user

        native_env = {**environment, "DUCKONWIRE_TOKEN": token}
        native_log = (fixture / "native.log").open("w")
        native_process = subprocess.Popen(
            [str(args.server), "--root", str(owner), "--port", str(available_port()), "--pg-port", "0",
             "--access-file", "private/access.tsv", "--tls-cert", "private/cert.pem", "--tls-key", "private/key.pem",
             "--query-timeout-ms", "3000", "--max-queries", "2"],
            env=native_env, stdout=native_log, stderr=native_log)
        descriptor_path = owner / ".runtime/native.json"
        deadline = time.monotonic() + 20
        while True:
            require(native_process.poll() is None, "Owned DuckOnWire server exited during startup")
            require(time.monotonic() < deadline, "Owned server startup timed out")
            try:
                descriptor = json.loads(descriptor_path.read_text())
                native_port = int(descriptor["pg_port"])
                break
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(0.05)

        native_options = dict(host="localhost", port=native_port, dbname="duckonwire", sslmode="verify-full",
                              sslrootcert=str(private / "ca.pem"), autocommit=True, prepare_threshold=None,
                              connect_timeout=10)
        writer_options = {**native_options, "user": "writer", "password": writer_password}
        reader_options = {**native_options, "user": "reader", "password": reader_password}
        pg_options = dict(host="127.0.0.1", port=pgport, dbname="postgres", autocommit=True,
                          prepare_threshold=None, connect_timeout=10)

        with psycopg.connect(**writer_options) as writer:
            writer.execute("CREATE TABLE products(id INTEGER PRIMARY KEY,name VARCHAR,brand VARCHAR,category VARCHAR,size VARCHAR,unit VARCHAR,country_code VARCHAR)")
            writer.execute("CREATE TABLE prices(id INTEGER PRIMARY KEY,product_id INTEGER,price DECIMAL(18,2),day DATE)")
            writer.execute("INSERT INTO products SELECT i::INTEGER,'Product '||i,'Example brand','Pantry','1','unit',CASE WHEN i%%2=0 THEN 'AR' ELSE 'ES' END FROM range(1,%s) t(i)", (args.rows + 1,))
            writer.execute("INSERT INTO prices SELECT i::INTEGER,i::INTEGER,CASE WHEN i=1 THEN 3.00 ELSE (i%%100+1)::DECIMAL(18,2) END,DATE '2026-01-01' FROM range(1,%s) t(i)", (args.rows + 1,))
            writer.execute("CREATE TABLE details(id INTEGER,enabled BOOLEAN,born DATE,payload BLOB,note VARCHAR,amount DECIMAL(18,2))")
            writer.execute("INSERT INTO details VALUES(%s,%s,%s,%s,%s,%s)",
                           (1, True, datetime.date(2026, 1, 2), b"\x00\xff\x01", None, decimal.Decimal("19.25")))
            report["engine"] = writer.execute("SELECT version()").fetchone()[0]

        def same_connection_read_write():
            with psycopg.connect(**writer_options) as writer:
                writer.execute("CREATE TABLE same_connection_inventory(id INTEGER PRIMARY KEY,stock INTEGER)")
                try:
                    writer.execute("BEGIN")
                    writer.execute("INSERT INTO same_connection_inventory VALUES(%s,%s)", (1, 3))
                    require(writer.execute("SELECT stock FROM same_connection_inventory WHERE id=%s", (1,)).fetchone() == (3,), "Writer cannot read its pending insert")
                    writer.execute("UPDATE same_connection_inventory SET stock=stock+1 WHERE id=%s", (1,))
                    require(writer.execute("SELECT stock FROM same_connection_inventory WHERE id=%s", (1,)).fetchone() == (4,), "Writer cannot read its pending update")
                    writer.execute("COMMIT")
                    writer.execute("BEGIN")
                    writer.execute("DELETE FROM same_connection_inventory WHERE id=%s", (1,))
                    require(writer.execute("SELECT count(*) FROM same_connection_inventory").fetchone() == (0,), "Writer cannot read its pending delete")
                    writer.execute("ROLLBACK")
                    require(writer.execute("SELECT stock FROM same_connection_inventory WHERE id=%s", (1,)).fetchone() == (4,), "Rollback did not restore the committed row")
                finally:
                    with contextlib.suppress(Exception):
                        writer.execute("ROLLBACK")
                    writer.execute("DROP TABLE same_connection_inventory")
            return {"connection_count": 1, "operations": ["SELECT", "INSERT", "UPDATE", "DELETE", "COMMIT", "ROLLBACK"]}
        check("same_connection_reads_and_writes_with_native_transactions", same_connection_read_write)

        def with_reader(callback):
            with psycopg.connect(**reader_options) as reader:
                return callback(reader)

        def protected_reader(reader):
            require(reader.execute("SELECT count(*) FROM products").fetchone()[0] == args.rows, "Reader count differs")
            expect_error(reader, "UPDATE prices SET price=99 WHERE id=1", "42501")
            require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Denied write mutated data")
        check("protected_reader_tls_and_write_denial", lambda: with_reader(protected_reader))

        def recover_snapshot(reader, bad_statement, error_state, *, prepare=None):
            with psycopg.connect(**writer_options) as writer:
                reader.execute("BEGIN")
                require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Initial snapshot differs")
                reader.execute("SAVEPOINT recovery", prepare=prepare)
                writer.execute("UPDATE prices SET price=4 WHERE id=1")
                try:
                    expect_error(reader, bad_statement, error_state, prepare=prepare)
                    expect_error(reader, "SELECT 1", "25P02")
                    reader.execute("ROLLBACK TO SAVEPOINT recovery", prepare=prepare)
                    require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Recovery restarted transaction or lost snapshot")
                    reader.execute("RELEASE SAVEPOINT recovery", prepare=prepare)
                    reader.execute("COMMIT")
                    require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("4.00"), "New snapshot did not see writer commit")
                finally:
                    with contextlib.suppress(Exception):
                        reader.execute("ROLLBACK")
                    writer.execute("UPDATE prices SET price=3 WHERE id=1")
        check("read_only_savepoint_runtime_error_preserves_snapshot", lambda: with_reader(
            lambda reader: recover_snapshot(reader, "SELECT CAST(name AS INTEGER) FROM products WHERE id=1", "22P02")))
        check("read_only_savepoint_parser_error_preserves_snapshot", lambda: with_reader(
            lambda reader: recover_snapshot(reader, "SELCT 1", "42601")))
        check("read_only_savepoint_extended_protocol_preserves_snapshot", lambda: with_reader(
            lambda reader: recover_snapshot(reader, "SELECT CAST(name AS INTEGER) FROM products WHERE id=1", "22P02", prepare=True)))

        def nested(reader):
            reader.execute("BEGIN")
            reader.execute('SAVEPOINT "Outer Point"')
            reader.execute("SAVEPOINT repeated")
            reader.execute("SAVEPOINT repeated")
            expect_error(reader, "SELECT CAST(name AS INTEGER) FROM products WHERE id=1", "22P02")
            reader.execute("ROLLBACK TO repeated")
            reader.execute("RELEASE repeated")
            reader.execute("ROLLBACK TO repeated")
            require(reader.execute("SELECT 42").fetchone()[0] == 42, "Duplicate savepoint levels were not independent")
            reader.execute('RELEASE SAVEPOINT "Outer Point"')
            expect_error(reader, "ROLLBACK TO repeated", "3B001")
            reader.execute("ROLLBACK")
        check("savepoint_nesting_duplicate_names_and_release", lambda: with_reader(nested))

        def cursor_rollback(reader):
            reader.execute("BEGIN")
            reader.execute("DECLARE retained CURSOR FOR SELECT i::INTEGER FROM range(4) t(i)")
            require(reader.execute("FETCH 1 FROM retained").fetchall() == [(0,)], "Outer cursor setup failed")
            reader.execute("SAVEPOINT cursors")
            reader.execute("DECLARE discarded CURSOR FOR SELECT 99")
            require(reader.execute("FETCH 1 FROM retained").fetchall() == [(1,)], "Outer cursor fetch failed")
            reader.execute("ROLLBACK TO cursors")
            require(reader.execute("FETCH 1 FROM retained").fetchall() == [(2,)], "Rollback rewound retained cursor")
            expect_error(reader, "FETCH 1 FROM discarded", "34000")
            reader.execute("ROLLBACK TO cursors")
            reader.execute("CLOSE retained")
            reader.execute("ROLLBACK TO cursors")
            expect_error(reader, "FETCH 1 FROM retained", "34000")
            reader.execute("ROLLBACK TO cursors")
            require(reader.execute("SELECT 43").fetchone()[0] == 43, "Cursor errors prevented recovery")
            reader.execute("COMMIT")
        check("savepoint_cursor_lifecycle_and_fetch_position", lambda: with_reader(cursor_rollback))

        def recover_denied_write(reader):
            reader.execute("BEGIN")
            reader.execute("SAVEPOINT permission")
            expect_error(reader, "UPDATE prices SET price=99 WHERE id=1", "42501")
            reader.execute("ROLLBACK TO permission")
            require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Write denial lost snapshot or mutated data")
            expect_error(reader, "CREATE TEMP TABLE forbidden(id INTEGER)", "42501")
            reader.execute("ROLLBACK TO permission")
            reader.execute("COMMIT")
        check("savepoint_recovery_does_not_enable_writes_or_temp_ddl", lambda: with_reader(recover_denied_write))

        def cancel_recovery(reader):
            with psycopg.connect(**writer_options) as writer:
                reader.execute("BEGIN")
                require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Cancellation snapshot setup failed")
                reader.execute("SAVEPOINT cancellation")
                writer.execute("UPDATE prices SET price=4 WHERE id=1")
                cancel_errors = []

                def cancel_later():
                    time.sleep(0.2)
                    try:
                        reader.cancel()
                    except Exception:
                        cancel_errors.append(True)

                cancel_thread = threading.Thread(target=cancel_later)
                cancel_thread.start()
                try:
                    expect_error(reader, "SELECT sum(sin(i::DOUBLE)) FROM range(1000000000) t(i)", "57014")
                    cancel_thread.join(5)
                    require(not cancel_thread.is_alive() and not cancel_errors, "Cancellation did not complete")
                    reader.execute("ROLLBACK TO cancellation")
                    require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Cancellation recovery lost retained snapshot")
                    reader.execute("ROLLBACK")
                finally:
                    cancel_thread.join(5)
                    with contextlib.suppress(Exception):
                        reader.execute("ROLLBACK")
                    writer.execute("UPDATE prices SET price=3 WHERE id=1")
        check("savepoint_cancel_recovery_preserves_snapshot", lambda: with_reader(cancel_recovery))

        def reject_writer_savepoint():
            with psycopg.connect(**writer_options) as writer:
                writer.execute("BEGIN")
                expect_error(writer, "SAVEPOINT unsupported_writer", "0A000")
                writer.execute("ROLLBACK")
                require(writer.execute("SELECT 44").fetchone()[0] == 44, "Writer savepoint rejection poisoned connection")
        check("writer_savepoints_rejected_without_fake_rollback", reject_writer_savepoint)

        tutorial_env = {
            **environment, "PGHOST": "127.0.0.1", "PGPORT": str(pgport), "PGDATABASE": "postgres",
            "DUCKONWIRE_FDW_HOST": "localhost", "DUCKONWIRE_FDW_PORT": str(native_port),
            "DUCKONWIRE_FDW_DATABASE": "duckonwire", "DUCKONWIRE_FDW_USER": "reader",
            "DUCKONWIRE_FDW_PASSWORD": reader_password, "DUCKONWIRE_FDW_SSLROOTCERT": str(private / "ca.pem"),
        }
        psql_command = [args.psql, "-X", "--no-psqlrc", "-v", "ON_ERROR_STOP=1"]
        check("actual_neon_setup_sql", lambda: run([*psql_command, "-f", args.examples / "setup.sql"], env=tutorial_env))
        check("actual_neon_query_sql", lambda: run([*psql_command, "-f", args.examples / "query.sql"], env=tutorial_env))

        with psycopg.connect(**pg_options) as postgres:
            def foreign_paging():
                rows = postgres.execute("SELECT id FROM duckonwire.products ORDER BY id").fetchall()
                require([row[0] for row in rows] == list(range(1, args.rows + 1)), "FDW paging lost, duplicated or reordered rows")
                return {"rows": len(rows), "foreign_fetch_size": 1000}
            check("postgres_fdw_complete_pagination", foreign_paging)

            def foreign_types():
                postgres.execute("CREATE FOREIGN TABLE duckonwire.details(id integer,enabled boolean,born date,payload bytea,note text,amount numeric(18,2)) SERVER duckonwire OPTIONS(schema_name 'main',table_name 'details')")
                row = postgres.execute("SELECT * FROM duckonwire.details").fetchone()
                require(row == (1, True, datetime.date(2026, 1, 2), b"\x00\xff\x01", None, decimal.Decimal("19.25")), "FDW type roundtrip changed values")
                postgres.execute("DROP FOREIGN TABLE duckonwire.details")
            check("postgres_fdw_typed_null_decimal_date_and_binary", foreign_types)

            def foreign_parameters():
                for value in [1, 1002, args.rows, 3, 4, 2]:
                    row = postgres.execute("SELECT id,name FROM duckonwire.products WHERE id=%s", (value,), prepare=True).fetchone()
                    require(row == (value, "Product " + str(value)), "Repeated prepared FDW query changed bound value")
            check("postgres_fdw_prepared_parameter_reuse", foreign_parameters)

            def local_foreign_complex_join():
                postgres.execute("CREATE TEMP TABLE local_categories(country_code text,label text)")
                postgres.execute("INSERT INTO local_categories VALUES('AR','Argentina'),('ES','Spain')")
                rows = postgres.execute("WITH priced AS (SELECT p.id,p.country_code,r.price,row_number() OVER(PARTITION BY p.country_code ORDER BY r.price DESC,p.id) AS rank FROM duckonwire.products p JOIN duckonwire.prices r ON r.product_id=p.id) SELECT l.label,count(*),sum(x.price) FROM priced x JOIN local_categories l USING(country_code) WHERE rank<=10 GROUP BY l.label ORDER BY l.label").fetchall()
                require(len(rows) == 2 and all(row[1] == 10 and row[2] > 0 for row in rows), "Local/foreign CTE/window/aggregate result differs")
            check("postgres_fdw_local_join_cte_window_aggregate", local_foreign_complex_join)

            def foreign_subtransaction_snapshot():
                with psycopg.connect(**writer_options) as writer:
                    postgres.execute("BEGIN")
                    require(postgres.execute("SELECT price FROM duckonwire.prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "FDW initial snapshot differs")
                    postgres.execute("SAVEPOINT foreign_child")
                    writer.execute("UPDATE prices SET price=4 WHERE id=1")
                    try:
                        plan = "\n".join(row[0] for row in postgres.execute("EXPLAIN (VERBOSE, COSTS FALSE) SELECT name::integer FROM duckonwire.products WHERE id=1"))
                        require("Remote SQL:" in plan and "integer" in plan, "Invalid cast was not pushed to remote reader")
                        expect_error(postgres, "SELECT name::integer FROM duckonwire.products WHERE id=1", "22P02")
                        postgres.execute("ROLLBACK TO foreign_child")
                        require(postgres.execute("SELECT price FROM duckonwire.prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "FDW subtransaction error recovery lost remote snapshot")
                        postgres.execute("RELEASE foreign_child")
                        postgres.execute("COMMIT")
                        require(postgres.execute("SELECT price FROM duckonwire.prices WHERE id=1").fetchone()[0] == decimal.Decimal("4.00"), "FDW new transaction missed writer commit")
                    finally:
                        with contextlib.suppress(Exception):
                            postgres.execute("ROLLBACK")
                        writer.execute("UPDATE prices SET price=3 WHERE id=1")
            check("postgres_fdw_real_subtransaction_error_preserves_snapshot", foreign_subtransaction_snapshot)

            def foreign_readonly():
                expect_error(postgres, "UPDATE duckonwire.prices SET price=99 WHERE id=1", "55000")
                postgres.execute("ALTER SERVER duckonwire OPTIONS (SET updatable 'true')")
                try:
                    expect_error(postgres, "UPDATE duckonwire.prices SET price=99 WHERE id=1", "42501")
                    require(postgres.execute("SELECT price FROM duckonwire.prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Remote reader denial changed data")
                finally:
                    postgres.execute("ALTER SERVER duckonwire OPTIONS (SET updatable 'false')")
            check("postgres_fdw_readonly_option_and_remote_role_denial", foreign_readonly)

        check("actual_neon_cleanup_sql", lambda: run([*psql_command, "-f", args.examples / "cleanup.sql"], env=tutorial_env))
        with psycopg.connect(**pg_options) as postgres:
            def cleanup_absent():
                require(postgres.execute("SELECT count(*) FROM pg_namespace WHERE nspname='duckonwire'").fetchone()[0] == 0, "Tutorial schema remains")
                require(postgres.execute("SELECT count(*) FROM pg_foreign_server WHERE srvname='duckonwire'").fetchone()[0] == 0, "Tutorial foreign server remains")
            check("tutorial_objects_removed_without_remote_data_changes", cleanup_absent)
        with psycopg.connect(**reader_options) as reader:
            require(reader.execute("SELECT count(*),min(id),max(id),sum(id) FROM products").fetchone() ==
                    (args.rows, 1, args.rows, args.rows * (args.rows + 1) // 2), "Fixture catalog integrity changed")
            require(reader.execute("SELECT price FROM prices WHERE id=1").fetchone()[0] == decimal.Decimal("3.00"), "Controlled snapshot mutation was not restored")
        report["controlled_snapshot_mutations_restored"] = True
        report["passed"] = all(item["passed"] for item in report["checks"])
    except Exception as error:
        if not report["checks"] or report["checks"][-1]["passed"]:
            message = str(error)
            for secret in credentials:
                message = message.replace(secret, "[redacted]")
            report["checks"].append({"name": "fixture_setup_or_teardown", "passed": False, "error": message[:500]})
        report["passed"] = False
    finally:
        cleanup_ok = True
        if native_process is not None and native_process.poll() is None:
            native_process.terminate()
            try:
                native_process.wait(10)
                cleanup_ok = native_process.returncode == 0
            except subprocess.TimeoutExpired:
                native_process.kill()
                native_process.wait(10)
                cleanup_ok = False
        if native_log is not None:
            native_log.close()
        if pg_started:
            try:
                run([args.pg_bin / "pg_ctl", "-D", pgdata, "-m", "fast", "-w", "stop"])
            except Exception:
                cleanup_ok = False
        # Remove only the exact temporary tree this process created, after both
        # owners have stopped. Logs may contain private connection descriptors.
        if cleanup_ok:
            shutil.rmtree(fixture)
        report["owned_fixture_cleanup_complete"] = cleanup_ok and not fixture.exists()
        report["passed"] = report["passed"] and report["owned_fixture_cleanup_complete"]
        if not cleanup_ok:
            report["checks"].append({"name": "owned_servers_graceful_cleanup", "passed": False})
    print(json.dumps(report, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
