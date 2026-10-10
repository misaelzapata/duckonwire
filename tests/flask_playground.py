"""Exercise the ordinary Flask client against an existing protected reader account."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import socket
from types import SimpleNamespace
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

WINDOW_TOP3 = 'WITH ranked AS (SELECT p.country_code,p.name,p.brand,pr.price,row_number() OVER(PARTITION BY p.country_code ORDER BY pr.price DESC NULLS LAST,p.id ASC NULLS LAST) AS position FROM products p JOIN prices pr ON pr.product_id=p.id) SELECT country_code,name,brand,price FROM ranked WHERE position<=3 ORDER BY country_code ASC NULLS LAST,price DESC NULLS LAST,name ASC NULLS LAST'
TOPK_FIXTURES = [('mixed_price_nulls', "WITH products(id,country_code,name,brand) AS (VALUES (1,'AR','Null first','A'),(2,'AR','High','B'),(3,'AR','Null third','C'),(4,'AR','Low','D')), prices(product_id,price) AS (VALUES (1,CAST(NULL AS DECIMAL(18,2))),(2,5.00),(3,NULL),(4,1.00)), "), ('equal_price_id_tiebreak', "WITH products(id,country_code,name,brand) AS (VALUES (1,'AR','Z-name','A'),(2,'AR','A-name','B'),(3,'AR','M-name','C'),(4,'AR','B-name','D')), prices(product_id,price) AS (VALUES (4,CAST(10.00 AS DECIMAL(18,2))),(2,10.00),(3,10.00),(1,10.00)), "), ('multiple_prices_same_product', "WITH products(id,country_code,name,brand) AS (VALUES (1,'ES','Repeated','A'),(2,'ES','Other','B')), prices(product_id,price) AS (VALUES (1,CAST(10.00 AS DECIMAL(18,2))),(1,10.00),(1,9.00),(2,8.00)), "), ('null_brand_payload', "WITH products(id,country_code,name,brand) AS (VALUES (1,'ES','One',CAST(NULL AS VARCHAR)),(2,'ES','Two',NULL),(3,'ES','Three',NULL),(4,'ES','Four','excluded')), prices(product_id,price) AS (VALUES (1,CAST(7.00 AS DECIMAL(18,2))),(2,6.00),(3,5.00),(4,4.00)), "), ('null_country_group', "WITH products(id,country_code,name,brand) AS (VALUES (1,CAST(NULL AS VARCHAR),'Null country one','A'),(2,NULL,'Null country two','B'),(3,'AR','Normal','C')), prices(product_id,price) AS (VALUES (1,CAST(7.00 AS DECIMAL(18,2))),(2,6.00),(3,5.00)), "), ('all_null_prices', "WITH products(id,country_code,name,brand) AS (VALUES (1,'AR','One','A'),(2,'AR','Two','B'),(3,'AR','Three','C'),(4,'AR','Four','D')), prices(product_id,price) AS (VALUES (4,CAST(NULL AS DECIMAL(18,2))),(2,NULL),(3,NULL),(1,NULL)), "), ('small_groups_and_null_names', "WITH products(id,country_code,name,brand) AS (VALUES (1,'AR',CAST(NULL AS VARCHAR),'A'),(2,'ES','Two','B'),(3,'ES','Three','C')), prices(product_id,price) AS (VALUES (1,CAST(7.00 AS DECIMAL(18,2))),(2,6.00),(3,5.00)), "), ('mixed_all_features', "WITH products(id,country_code,name,brand) AS (VALUES (1,CAST(NULL AS VARCHAR),'Null country',NULL),(2,NULL,'Other null country','B'),(3,NULL,'Third null country',NULL),(4,NULL,'Fourth null country','D'),(5,'AR','Tie high id','E'),(6,'AR','Tie higher id',NULL),(7,'ES','Small group',NULL)), prices(product_id,price) AS (VALUES (1,CAST(NULL AS DECIMAL(18,2))),(2,9.00),(2,9.00),(3,9.00),(4,NULL),(5,10.00),(6,10.00),(7,NULL)), ")]


def maintenance_checks(maintenance):
    """Exercise the real official check against actual authenticated native sockets."""
    interval = maintenance.POOL_CHECK_INTERVAL_SECONDS
    maintenance.POOL_CHECK_INTERVAL_SECONDS = 0.02
    try:
        with ConnectionPool(kwargs=connection_settings() | {"prepare_threshold": None},
                            min_size=2, max_size=2, num_workers=1, open=False) as isolated:
            isolated.wait(timeout=5)
            first, second = isolated.getconn(), isolated.getconn()
            identities = {first.info.backend_pid, second.info.backend_pid}
            isolated.putconn(first)
            isolated.putconn(second)
            verified = set()
            all_checked = threading.Event()
            def actual_check(connection):
                ConnectionPool.check_connection(connection)
                verified.add(connection.info.backend_pid)
                if len(verified) == 2:
                    all_checked.set()
            isolated.check_connection = actual_check
            stop = threading.Event()
            monitor = threading.Thread(target=maintenance._maintain_pool,
                                       args=(isolated, stop), daemon=True)
            created = isolated.get_stats()["connections_num"]
            monitor.start()
            try:
                check("periodic official health checks cover both idle authenticated sessions",
                      all_checked.wait(timeout=3) and verified == identities
                      and isolated.get_stats()["connections_num"] == created)
            finally:
                stop.set()
                monitor.join(timeout=3)
            check("maintenance stop interrupts the periodic wait without residual work", not monitor.is_alive())
            # Break only our idle client sockets without telling the pool. This matches
            # a peer idle timeout: a Connection remains open until the next native check.
            for connection in (first, second):
                duplicate = socket.socket(fileno=os.dup(connection.fileno()))
                try:
                    duplicate.shutdown(socket.SHUT_RDWR)
                finally:
                    duplicate.close()
            verified.clear()
            all_checked.clear()
            stop = threading.Event()
            monitor = threading.Thread(target=maintenance._maintain_pool,
                                       args=(isolated, stop), daemon=True)
            monitor.start()
            try:
                check("maintenance replaces both peer-closed idle sockets before a user checkout",
                      all_checked.wait(timeout=5) and len(verified) == 2
                      and not verified.intersection(identities))
            finally:
                stop.set()
                monitor.join(timeout=3)
            ready_first, ready_second = isolated.getconn(), isolated.getconn()
            try:
                check("replacement pool remains bounded and yields two clean native sessions",
                      {ready_first.info.backend_pid, ready_second.info.backend_pid} == verified
                      and ready_first.info.transaction_status == ready_second.info.transaction_status == TransactionStatus.IDLE
                      and isolated.max_size == 2)
            finally:
                isolated.putconn(ready_first)
                isolated.putconn(ready_second)
            entered, release = threading.Event(), threading.Event()
            def blocked_check(connection):
                entered.set()
                if not release.wait(timeout=3):
                    raise psycopg.OperationalError("Test health-check barrier timed out")
                ConnectionPool.check_connection(connection)
            isolated.check_connection = blocked_check
            stop = threading.Event()
            monitor = threading.Thread(target=maintenance._maintain_pool,
                                       args=(isolated, stop), daemon=True)
            monitor.start()
            try:
                held = False
                if entered.wait(timeout=2):
                    try:
                        early = isolated.getconn(timeout=0.05)
                        isolated.putconn(early)
                    except PoolTimeout:
                        held = True
                check("official health check withholds idle sockets until verification finishes", held)
            finally:
                release.set()
                stop.set()
                monitor.join(timeout=3)
            ready = isolated.getconn(timeout=3)
            try:
                check("health-check barrier releases the same verified idle session",
                      ready.info.backend_pid in verified
                      and ready.info.transaction_status == TransactionStatus.IDLE)
            finally:
                isolated.putconn(ready)
        stop = threading.Event()
        monitor = threading.Thread(target=maintenance._maintain_pool,
                                   args=(isolated, stop), daemon=True)
        monitor.start()
        monitor.join(timeout=1)
        check("maintenance exits when its original pool is closed", not monitor.is_alive())
        worker = SimpleNamespace()
        maintenance.post_worker_init(worker)
        thread = worker._pool_health_thread
        maintenance.post_worker_init(worker)
        check("post-fork worker initialization creates exactly one maintenance thread",
              thread is worker._pool_health_thread and thread.is_alive()
              and len([item for item in threading.enumerate() if item.name == "pool-health"]) == 1)
        maintenance.worker_exit(None, worker)
        check("worker exit stops maintenance and closes its pool", worker._pool_health_stop.is_set()
              and not thread.is_alive() and not any(item.name == "pool-health" for item in threading.enumerate()))
    finally:
        maintenance.POOL_CHECK_INTERVAL_SECONDS = interval


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
    spec = importlib.util.spec_from_file_location("playground_gunicorn", EXAMPLE / "gunicorn.conf.py")
    maintenance = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(maintenance)
    check("module import creates neither a pool nor pre-fork maintenance threads",
          __import__("app")._pool is None
          and not any(item.name == "pool-health" for item in threading.enumerate()))
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
        if preset["id"] in ("history_lag", "seven_day_average", "brand_movement"):
            check("actual catalog preset " + preset["id"], response.status_code == 200
                  and 1 <= body["row_count"] <= 5 and body["read_only"])
            continue
        check("actual catalog preset " + preset["id"], response.status_code == 200
              and body["row_count"] == (2 if preset["id"] == "countries" else 5)
              and body["truncated"] == (preset["id"] != "countries") and body["read_only"])
    top = next(preset["sql"] for preset in PRESETS if preset["id"] == "top")
    example_top = (ROOT / "examples/postgres/queries/top-prices.sql").read_text()
    def answer(source):
        response = client.post("/query", json={"sql": source, "limit": 200})
        if response.status_code != 200:
            raise AssertionError("TopK query failed")
        body = response.get_json()
        return {key: body[key] for key in ("columns", "rows", "row_count", "truncated", "read_only")}
    reference = answer(WINDOW_TOP3)
    check("TopK preset and public SQL match all six window-ranked catalog rows",
          reference["row_count"] == 6 and answer(top) == reference and answer(example_top) == reference)
    for name, prefix in TOPK_FIXTURES:
        check("actual TopK/window equivalence for " + name,
              answer(prefix + top.removeprefix("WITH ")) == answer(prefix + WINDOW_TOP3.removeprefix("WITH ")))
    boundaries = (
        "WITH products(id,country_code,name,brand) AS (VALUES "
        "(2147483647,'AR','Higher price','A'),(-2147483648,'AR','Lower price','B'),"
        "(0,'AR','Middle id','C'),(1,'AR','Other middle id','D')), "
        "prices(product_id,price) AS (VALUES "
        "(2147483647,CAST(250.00 AS DECIMAL(18,2))),(-2147483648,249.99),(0,250.00),(1,250.00)), "
    )
    check("TopK cents outweigh the full signed product-id range",
          answer(boundaries + top.removeprefix("WITH ")) ==
          answer(boundaries + WINDOW_TOP3.removeprefix("WITH ")))
    ties = boundaries.replace("249.99", "250.00")
    check("TopK equal prices retain signed product-id ordering",
          answer(ties + top.removeprefix("WITH ")) ==
          answer(ties + WINDOW_TOP3.removeprefix("WITH ")))
    histories = (
        "WITH products(id,name,brand,country_code) AS (VALUES (1,'One','A','AR'),(2,'Two','A','AR')), "
        "prices(id,product_id,price,day) AS (VALUES "
        "(1,1,CAST(10.00 AS DECIMAL(18,2)),DATE '2026-01-01'),"
        "(2,1,14.00,DATE '2026-01-03'),(3,1,20.00,DATE '2026-01-10'),"
        "(4,2,40.00,DATE '2026-01-01'),(5,2,50.00,DATE '2026-01-03')), "
    )
    complex_presets = {preset["id"]: preset["sql"] for preset in PRESETS}
    history = answer(histories + complex_presets["history_lag"].removeprefix("WITH "))
    check("history compares each sample with its actual previous observation",
          history["rows"] == [[1, "One", "A", "2026-01-01", "10.00", None, None],
                              [2, "One", "A", "2026-01-03", "14.00", "10.00", "4.00"],
                              [3, "One", "A", "2026-01-10", "20.00", "14.00", "6.00"]])
    trend = answer(histories + complex_presets["seven_day_average"].removeprefix("WITH "))
    check("seven-day average weights observations and excludes calendar gaps",
          trend["rows"] == [["AR", "2026-01-01", 2, 25.0, 25.0],
                            ["AR", "2026-01-03", 2, 32.0, 28.5],
                            ["AR", "2026-01-10", 1, 20.0, 20.0]])
    movement = answer(histories + complex_presets["brand_movement"].removeprefix("WITH "))
    check("brand movement counts products and compares first and last prices",
          movement["rows"] == [["AR", "A", 2, 10.0, "2"]])
    movement_reference = (
        "WITH ranked AS (SELECT product_id,price,"
        "row_number() OVER(PARTITION BY product_id ORDER BY day ASC NULLS LAST,id ASC) AS first_row,"
        "row_number() OVER(PARTITION BY product_id ORDER BY day DESC NULLS LAST,id DESC) AS last_row "
        "FROM prices), samples AS (SELECT product_id,"
        "max(price) FILTER(WHERE first_row=1) AS first_price,"
        "max(price) FILTER(WHERE last_row=1) AS last_price FROM ranked GROUP BY product_id) "
        "SELECT p.country_code,p.brand,count(*) AS products_with_history,"
        "round(avg(s.last_price-s.first_price),2) AS average_change,"
        "count_if(s.last_price>s.first_price) AS products_increased "
        "FROM samples s JOIN products p ON p.id=s.product_id GROUP BY p.country_code,p.brand "
        "ORDER BY average_change DESC NULLS LAST,p.country_code ASC NULLS LAST,p.brand ASC NULLS LAST"
    )
    movement_edges = (
        "WITH products(id,name,brand,country_code) AS (VALUES (1,'One','A','AR'),(2,'Two','B','ES')), "
        "prices(id,product_id,price,day) AS (VALUES "
        "(CAST('-9223372036854775808' AS BIGINT),1,CAST(1.00 AS DECIMAL(18,2)),DATE '-infinity'),"
        "(9223372036854775807,1,2.00,DATE '-infinity'),(0,1,4.00,DATE 'infinity'),"
        "(1,1,8.00,NULL),(2,2,3.00,DATE '1970-01-01'),(3,2,NULL,DATE 'infinity')), "
    )
    for name, prefix in (("infinite dates and extreme ids", movement_edges),
                         ("null dates and prices", movement_edges.replace("DATE '-infinity'", "CAST(NULL AS DATE)")
                          .replace("DATE 'infinity'", "CAST(NULL AS DATE)")
                          .replace("DATE '1970-01-01'", "CAST(NULL AS DATE)"))):
        check("brand movement matches ordered history for " + name,
              answer(prefix + complex_presets["brand_movement"].removeprefix("WITH ")) ==
              answer(prefix + movement_reference.removeprefix("WITH ")))
    response = client.post("/query", json={"sql": "SELECT i FROM range(10000) t(i)", "limit": 200})
    body = response.get_json()
    check("database-side cap preserves exactly 200 requested rows", response.status_code == 200
          and body["row_count"] == 200 and body["truncated"]
          and body["rows"] == [[i] for i in range(200)])
    for limit in (0, 201, True, "200"):
        check("invalid row limit rejected " + str(limit),
              client.post("/query", json={"sql": "SELECT 42", "limit": limit}).status_code == 400)
    paginated = "SELECT i FROM range(23) t(i) ORDER BY i"
    page_rows = []
    for offset, size, next_offset in ((0, 10, 10), (10, 10, 20), (20, 3, None)):
        response = client.post("/query", json={"sql": paginated, "limit": 10, "offset": offset})
        body = response.get_json()
        check("bound pagination returns the exact ordered page at " + str(offset),
              response.status_code == 200 and body["rows"] == [[i] for i in range(offset, offset + size)]
              and body["row_count"] == size and body["limit"] == 10 and body["offset"] == offset
              and body["next_offset"] == next_offset and body["truncated"] == (next_offset is not None))
        page_rows.extend(body["rows"])
    check("successive pages contain all rows without duplicates or gaps", page_rows == [[i] for i in range(23)])
    response = client.post("/query", json={"sql": paginated, "limit": 10, "offset": 23})
    check("an exhausted page is empty and has no next page", response.status_code == 200
          and response.get_json()["rows"] == [] and response.get_json()["next_offset"] is None)
    for offset in (-1, 1_000_001, True, "10", 1.5, None):
        check("invalid pagination offset rejected " + str(offset),
              client.post("/query", json={"sql": "SELECT 42", "offset": offset}).status_code == 400)
    response = client.post("/query", json={"sql": "SELECT i FROM range(1000210) t(i) ORDER BY i",
                                         "limit": 200, "offset": 1_000_000})
    check("the pagination boundary retains the row cap and ends navigation", response.status_code == 200
          and response.get_json()["row_count"] == 200 and response.get_json()["truncated"]
          and response.get_json()["next_offset"] is None)
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
        app.config["MAX_RESULT_BYTES"] = 600
        check("Unicode escaping cannot exceed the actual response byte cap",
              client.post("/query", json={"sql": "SELECT repeat('é',100) AS value"}).status_code == 400)
        app.config["MAX_RESULT_BYTES"] = 1500
        unicode_response = client.post("/query", json={"sql": "SELECT repeat('é',100) AS value"})
        check("bounded Unicode results retain their values after actual JSON encoding",
              unicode_response.status_code == 200 and len(unicode_response.data) <= 1500
              and unicode_response.get_json()["rows"] == [["é" * 100]])
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
    maintenance_checks(maintenance)


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
