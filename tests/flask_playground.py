"""Exercise the ordinary Flask client against an existing protected reader account."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import threading
import uuid

from psycopg_pool import ConnectionPool, PoolTimeout
from psycopg.pq import TransactionStatus

import psycopg

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples" / "postgres" / "flask"
sys.path.insert(0, str(EXAMPLE))
from app import PRESETS, app, close_pool, connection_pool, connection_settings, reset_connection, warm_pool  # noqa: E402

checks = []


def check(name, passed):
    checks.append({"name": name, "passed": bool(passed)})
    if not passed:
        raise AssertionError(name)


def run():
    if not os.environ.get("PGREADER_USER") or not os.environ.get("PGREADER_PASSWORD"):
        raise RuntimeError("Configure a protected PGREADER_USER and PGREADER_PASSWORD")
    os.environ["PGUSER"] = os.environ["PGREADER_USER"]
    os.environ["PGPASSWORD"] = os.environ["PGREADER_PASSWORD"]
    check("TLS hostname and certificate verification enabled",
          connection_settings()["sslmode"] == "verify-full")
    warm_pool()
    client = app.test_client()
    page = client.get("/")
    check("HTML page renders without backend credentials", page.status_code == 200
          and os.environ["PGPASSWORD"].encode() not in page.data
          and b"Query the catalog." in page.data and b'id="details"' in page.data and b'id="tables"' in page.data
          and b"unsafe-inline" not in page.headers.get("Content-Security-Policy", "").encode()
          and client.get("/static/duckonwire-logo.png").mimetype == "image/png"
          and client.get("/static/app.js").status_code == 200
          and client.get("/static/app.css").status_code == 200)
    catalog = client.get("/tables")
    catalog_body = catalog.get_json()
    check("catalog lists actual protected database tables without credentials", catalog.status_code == 200
          and catalog_body["read_only"] and any(item["name"] == "products" for item in catalog_body["tables"])
          and any(item["name"] == "prices" for item in catalog_body["tables"])
          and os.environ["PGPASSWORD"].encode() not in catalog.data)
    products = next(item for item in catalog_body["tables"] if item["name"] == "products")
    details = client.get("/table", query_string={"schema": products["schema"], "name": products["name"]})
    detail_body = details.get_json()
    check("table details contain native columns and bounded sample preview", details.status_code == 200
          and [column["name"] for column in detail_body["columns"]] ==
          ["id", "name", "brand", "category", "size", "unit", "country_code"]
          and detail_body["preview"]["row_count"] == 10 and detail_body["preview"]["truncated"]
          and all(isinstance(column["nullable"], bool) for column in detail_body["columns"]))
    check("table preview uses the same read-only response contract", detail_body["preview"]["read_only"]
          and detail_body["preview"]["elapsed_ms"] >= 0)
    check("catalog cannot accept backend connection overrides", client.get("/tables?host=external.example").status_code == 400)
    check("unknown tables have sanitized not-found responses", client.get("/table", query_string={"schema": "main", "name": "missing_private_table"}).status_code == 404)
    check("identifier injection does not become executable SQL", client.get("/table", query_string={"schema": "main", "name": 'products"; DELETE FROM products;--'}).status_code == 404)
    check("duplicate identifier query arguments rejected", client.get("/table?schema=main&name=products&name=prices").status_code == 400)
    check("control characters rejected before identifier binding", client.get("/table", query_string={"schema": "main", "name": "products\x00"}).status_code == 400)
    if os.environ.get("FLASK_QUOTED_TABLE"):
        quoted = client.get("/table", query_string={"schema": "main", "name": os.environ["FLASK_QUOTED_TABLE"]})
        check("real unusual identifiers are safely quoted for sample previews", quoted.status_code == 200
              and quoted.get_json()["preview"]["rows"] == [[42]]
              and quoted.get_json()["columns"][0]["name"] == 'column"name')
    for preset in PRESETS:
        response = client.post("/query", json={"sql": preset["sql"], "limit": 5})
        body = response.get_json()
        check("actual catalog preset " + preset["id"], response.status_code == 200
              and body["row_count"] == (2 if preset["id"] == "countries" else 5)
              and body["truncated"] == (preset["id"] != "countries") and body["read_only"])
    response = client.post("/query", json={"sql": "SELECT i FROM range(10000) t(i)", "limit": 200})
    body = response.get_json()
    check("database-side cap preserves exactly 200 requested rows", response.status_code == 200
          and body["row_count"] == 200 and body["truncated"]
          and body["rows"] == [[i] for i in range(200)])
    for limit in (0, 201, True, "200"):
        check("invalid row limit rejected " + str(limit),
              client.post("/query", json={"sql": "SELECT 42", "limit": limit}).status_code == 400)
    response = client.post("/query", json={"sql": "SELECT 19.99::DECIMAL(18,2), "
                          "DATE '2026-10-09', true, 42::BIGINT, from_hex('0001ff'), "
                          "'<script>alert(1)</script>'"})
    check("exact decimal, date, boolean, integer, blob and literal HTML output",
          response.status_code == 200 and response.get_json()["rows"] ==
          [["19.99", "2026-10-09", True, 42, "0001ff", "<script>alert(1)</script>"]])
    response = client.post("/query", json={"sql": "SELECT 'NaN'::DOUBLE, 'Infinity'::DOUBLE"})
    check("nonfinite native floats produce valid browser JSON", response.status_code == 200
          and response.get_json()["rows"] == [["nan", "inf"]])
    response = client.post("/query", json={"sql": "SELECT * FROM read_csv_auto('/etc/passwd')"})
    check("native reader rule rejects external file SQL", response.status_code == 403
          and response.get_json()["sqlstate"] == "42501")
    check("multiple statements cannot escape the derived query",
          client.post("/query", json={"sql": "SELECT 42; DELETE FROM products"}).status_code == 400)
    response = client.post("/query", json={"sql": "SELECT nonexistent_private_column FROM products"})
    check("SQL errors are sanitized", response.status_code == 400
          and b"nonexistent_private_column" not in response.data and b"Traceback" not in response.data)
    check("browser cannot replace backend connection settings",
          client.post("/query", json={"sql": "SELECT 42", "host": "external.example"}).status_code == 400)
    with psycopg.connect(**connection_settings()) as connection:
        rejected = False
        try:
            connection.execute("DELETE FROM products")
        except psycopg.Error as error:
            rejected = error.sqlstate == "42501"
            connection.rollback()
        check("real reader denies mutation independently of Flask", rejected)
    wide_sql = "SELECT " + ",".join("1 AS column_" + str(index) for index in range(129))
    check("result column count is bounded", client.post("/query", json={"sql": wide_sql}).status_code == 400)
    original_bytes = app.config["MAX_RESULT_BYTES"]
    try:
        app.config["MAX_RESULT_BYTES"] = 100
        check("serialized result bytes are bounded", client.post("/query", json={"sql": "SELECT repeat('x',200)"}).status_code == 400)
    finally:
        app.config["MAX_RESULT_BYTES"] = original_bytes
    # Exercise the actual reset callback with an official one-slot pool: it must never
    # return the session before resetting it, and failed resets must replace sockets.
    reset_entered, finish_reset = threading.Event(), threading.Event()
    def blocked_reset(connection):
        reset_entered.set()
        if not finish_reset.wait(timeout=3):
            raise psycopg.OperationalError("Test reset barrier timed out")
        reset_connection(connection)
    with ConnectionPool(kwargs=connection_settings() | {"prepare_threshold": None},
                        min_size=1, max_size=1, num_workers=1,
                        reset=blocked_reset, open=False) as isolated:
        isolated.wait(timeout=5)
        leased = isolated.getconn()
        identity = leased.info.backend_pid
        leased.execute("SELECT 42").fetchall()
        leased.rollback()
        isolated.putconn(leased)
        try:
            check("background reset starts before another checkout", reset_entered.wait(timeout=2))
            held = False
            try:
                early = isolated.getconn(timeout=0.05)
                isolated.putconn(early)
            except PoolTimeout:
                held = True
            check("sessions remain unavailable while their full reset is incomplete", held)
        finally:
            finish_reset.set()
        ready = isolated.getconn(timeout=3)
        try:
            check("completed reset releases the same clean idle session",
                  ready.info.backend_pid == identity
                  and ready.info.transaction_status == TransactionStatus.IDLE)
        finally:
            isolated.putconn(ready)
    failed_once = threading.Event()
    def failing_reset(connection):
        if not failed_once.is_set():
            failed_once.set()
            raise psycopg.OperationalError("Expected controlled reset failure")
        reset_connection(connection)
    with ConnectionPool(kwargs=connection_settings() | {"prepare_threshold": None},
                        min_size=1, max_size=1, num_workers=1,
                        reset=failing_reset, open=False) as isolated:
        isolated.wait(timeout=5)
        leased = isolated.getconn()
        identity = leased.info.backend_pid
        isolated.putconn(leased)
        replacement = isolated.getconn(timeout=5)
        try:
            check("failed background reset retires and replaces the native session",
                  failed_once.is_set() and replacement.info.backend_pid != identity
                  and replacement.info.transaction_status == TransactionStatus.IDLE)
        finally:
            isolated.putconn(replacement)
    pool = connection_pool()
    first, second = pool.getconn(), pool.getconn()
    try:
        bounded = False
        started = time.monotonic()
        try:
            unexpected = pool.getconn(timeout=0.05)
            pool.putconn(unexpected)
        except PoolTimeout:
            bounded = True
        check("bounded pool rejects an exhausted checkout promptly", bounded and time.monotonic() - started < 1)
        check("normal requests return idle sessions and disable automatic prepare",
              first.info.transaction_status == TransactionStatus.IDLE
              and second.info.transaction_status == TransactionStatus.IDLE
              and first.prepare_threshold is None and second.prepare_threshold is None)
    finally:
        pool.putconn(first)
        pool.putconn(second)
    blocker = pool.getconn()
    try:
        def session_id():
            answer = client.post("/query", json={"sql": "SELECT 42"})
            if answer.status_code != 200:
                raise AssertionError("session query failed")
            leased = pool.getconn()
            try:
                return leased.info.backend_pid
            finally:
                pool.putconn(leased)
        identity = session_id()
        created = pool.get_stats()["connections_num"]
        check("successive requests reuse one authenticated native session",
              session_id() == identity and session_id() == identity
              and pool.get_stats()["connections_num"] == created)
        response = client.post("/query", json={"sql": "SELECT 'not_an_integer'::INTEGER"})
        check("execution errors reset the session before its next request",
              response.status_code == 400 and response.get_json()["sqlstate"] == "22P02"
              and session_id() == identity)
        if os.environ.get("FLASK_TEST_OWNED_DB") == "yes":
            table = "flask_snapshot_" + uuid.uuid4().hex
            writer_settings = connection_settings() | {
                "user": os.environ["FLASK_TEST_WRITER_USER"],
                "password": os.environ["FLASK_TEST_WRITER_PASSWORD"], "autocommit": True}
            with psycopg.connect(**writer_settings) as writer:
                writer.execute(psycopg.sql.SQL("CREATE TABLE {}(value INTEGER)").format(psycopg.sql.Identifier(table)))
                try:
                    writer.execute(psycopg.sql.SQL("INSERT INTO {} VALUES(%s)").format(psycopg.sql.Identifier(table)), (3,))
                    query = psycopg.sql.SQL("SELECT value FROM {}").format(psycopg.sql.Identifier(table)).as_string(writer)
                    old = client.post("/query", json={"sql": query})
                    writer.execute(psycopg.sql.SQL("UPDATE {} SET value=%s").format(psycopg.sql.Identifier(table)), (4,))
                    new = client.post("/query", json={"sql": query})
                    check("reused connection starts a fresh snapshot for each request",
                          old.status_code == new.status_code == 200
                          and old.get_json()["rows"] == [[3]] and new.get_json()["rows"] == [[4]]
                          and session_id() == identity)
                finally:
                    writer.execute(psycopg.sql.SQL("DROP TABLE {}").format(psycopg.sql.Identifier(table)))
        if os.environ.get("FLASK_TEST_NATIVE_TIMEOUT_SECONDS"):
            response = client.post("/query", json={"sql": "SELECT sum(a.i*b.i) "
                                   "FROM range(100000000) a(i), range(100000000) b(i)"})
            replacement = session_id()
            check("native watchdog cancellation retires and replaces the session",
                  response.status_code == 408 and response.get_json()["sqlstate"] == "57014"
                  and replacement != identity)
            identity = replacement
        original_timeout = app.config["QUERY_TIMEOUT_SECONDS"]
        try:
            app.config["QUERY_TIMEOUT_SECONDS"] = 0.15
            before = time.monotonic()
            response = client.post("/query", json={"sql": "SELECT sum(a.i*b.i) "
                                   "FROM range(100000000) a(i), range(100000000) b(i)"})
            check("expensive query is canceled and its native session retired",
                  response.status_code == 408 and time.monotonic() - before < 5)
        finally:
            app.config["QUERY_TIMEOUT_SECONDS"] = original_timeout
        replacement = session_id()
        time.sleep(0.2)
        check("replacement session survives after the completed cancellation timer",
              replacement != identity and session_id() == replacement)
        # A stale socket is never handed to a user query: the official checkout check replaces it.
        released = pool.getconn()
        released.close()
        pool.putconn(released)
        check("closed pooled sockets are replaced before executing a new request", session_id() != replacement)
    finally:
        pool.putconn(blocker)
    response = client.post("/query", json={"sql": "SELECT count(*) FROM products"})
    check("connection reusable and catalog unchanged after errors and cancellation",
          response.status_code == 200 and response.get_json()["rows"] == [[46462]])


if __name__ == "__main__":
    result = {"scope": "Ordinary Flask/psycopg client against an existing TLS/SCRAM reader role; "
              "no server orchestration or engine build", "driver": "psycopg",
              "driver_version": psycopg.__version__, "pool_driver": "psycopg_pool",
              "pool_version": __import__("psycopg_pool").__version__, "checks": checks}
    try:
        run()
        result.update(passed=True, native_reader_verified=True, rows_limit=200,
                      source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted(EXAMPLE.rglob("*"))
                                     if p.is_file() and "__pycache__" not in p.parts})
    except Exception as error:
        result.update(passed=False, error_type=type(error).__name__)
    finally:
        close_pool()
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["passed"] else 1)
