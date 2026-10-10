"""Read-only Flask example. Configure a protected reader account using PG* env vars."""
import atexit
import datetime
import decimal
import json
import math
import os
import threading
import time
import uuid

from flask import Flask, jsonify, render_template, request
import psycopg
from psycopg import sql
from psycopg_pool import ConnectionPool, PoolClosed, PoolTimeout, TooManyRequests

app = Flask(__name__)
app.config.update(MAX_CONTENT_LENGTH=16_384, QUERY_TIMEOUT_SECONDS=28,
                  MAX_RESULT_BYTES=2_000_000, MAX_RESULT_COLUMNS=128)

PRESETS = [
    {"id": "products", "name": "Browse products", "hint": "Public brands from Argentina and Spain",
     "sql": "SELECT id, name, brand, country_code, size, unit\nFROM products\nORDER BY id"},
    {"id": "prices", "name": "See sample prices", "hint": "Generated prices, not retail offers",
     "sql": "SELECT p.name, p.brand, p.country_code, pr.price, pr.day\nFROM products p\nJOIN prices pr ON pr.product_id = p.id\nORDER BY p.id"},
    {"id": "brands", "name": "Compare brands", "hint": "A grouped join over the catalog",
     "sql": "SELECT p.country_code, p.brand, count(*) AS products,\n       round(avg(pr.price), 2) AS average_price\nFROM products p JOIN prices pr ON pr.product_id = p.id\nGROUP BY p.country_code, p.brand\nORDER BY products DESC, p.brand"},
    {"id": "window", "name": "Market comparison", "hint": "Window functions over sample prices",
     "sql": "SELECT p.name, p.brand, p.country_code, pr.price,\n       round(avg(pr.price) OVER (PARTITION BY p.country_code), 2) AS market_average\nFROM products p JOIN prices pr ON pr.product_id = p.id\nORDER BY p.id"},
    {"id": "latest", "name": "Latest prices", "hint": "A join with the newest sample price per product",
     "sql": "WITH latest AS (\n  SELECT product_id, price, day,\n         row_number() OVER (PARTITION BY product_id ORDER BY day DESC, id DESC) AS position\n  FROM prices WHERE product_id <= 500\n)\nSELECT p.id, p.name, p.brand, r.price, r.day\nFROM products p JOIN latest r ON r.product_id = p.id AND r.position = 1\nWHERE p.id <= 500 ORDER BY p.id"},
    {"id": "countries", "name": "Products by country", "hint": "Count the catalog's two markets",
     "sql": "SELECT country_code, count(*) AS products\nFROM products GROUP BY country_code ORDER BY country_code"},
]


def connection_settings():
    # The browser never supplies connection settings. Use a native read-only rule.
    if os.environ.get("PGSSLMODE", "verify-full") != "verify-full":
        raise RuntimeError("This example requires verified TLS and a protected reader account")
    if not os.environ.get("PGPASSWORD"):
        raise RuntimeError("Configure the reader PGPASSWORD")
    settings = {
        "host": os.environ.get("PGHOST", "127.0.0.1"),
        "port": int(os.environ.get("PGPORT", "9496")),
        "dbname": os.environ.get("PGDATABASE", "duckonwire"),
        "user": os.environ.get("PGUSER", "reader"),
        "password": os.environ["PGPASSWORD"],
        "sslmode": "verify-full", "sslrootcert": os.environ.get("PGSSLROOTCERT", "system"),
        "connect_timeout": 5,
    }
    if os.environ.get("PGHOSTADDR"):
        settings["hostaddr"] = os.environ["PGHOSTADDR"]
    return settings


_pool = None
_pool_pid = None
_pool_lock = threading.Lock()


def reset_connection(connection):
    # The official pool withholds this session until the full reset finishes.
    connection.autocommit = True
    try:
        connection.execute("DISCARD ALL")
    finally:
        connection.autocommit = False


def connection_pool():
    # Importing the app never opens sockets: each Gunicorn worker starts its own pool.
    global _pool, _pool_pid
    with _pool_lock:
        if _pool is not None and _pool_pid != os.getpid():
            raise RuntimeError("Initialize the pool after the worker forks")
        if _pool is None:
            settings = connection_settings()
            settings["prepare_threshold"] = None  # DISCARD ALL clears server-side statements.
            _pool = ConnectionPool(kwargs=settings, min_size=2, max_size=2,
                                   timeout=3, max_waiting=2, max_lifetime=3600, max_idle=60,
                                   reconnect_timeout=5, num_workers=1,
                                   check=ConnectionPool.check_connection, reset=reset_connection,
                                   name="playground", open=False)
            _pool_pid = os.getpid()
            _pool.open()
        return _pool


def warm_pool():
    connection_pool().wait(timeout=5)


def close_pool():
    global _pool, _pool_pid
    with _pool_lock:
        pool, _pool = _pool, None
        _pool_pid = None
    if pool is not None:
        pool.close(timeout=3)


atexit.register(close_pool)


def cell(value):
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, (datetime.date, datetime.time, datetime.datetime)):
        return value.isoformat()
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)[:2_000].hex()
    if isinstance(value, (decimal.Decimal, uuid.UUID)):
        return str(value)
    return str(value)[:4_000]


@app.after_request
def response_headers(response):
    response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["Referrer-Policy"] = "same-origin"
    response.headers["Content-Security-Policy"] = (
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'self'"
    )
    return response


@app.get("/")
def index():
    return render_template("index.html", presets=PRESETS,
                           label=os.environ.get("DUCKONWIRE_DEMO_LABEL", "Read-only playground"))


@app.get("/health")
def health():
    return jsonify(ok=True)


@app.errorhandler(413)
def too_large(_error):
    return jsonify(error="The query request is too large."), 413


@app.post("/query")
def query():
    if request.headers.get("Sec-Fetch-Site") not in (None, "none", "same-origin"):
        return jsonify(error="Send queries from this page."), 403
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict) or set(payload) - {"sql", "limit"}:
        return jsonify(error="Send a JSON object with sql and an optional limit."), 400
    source = payload.get("sql", "")
    limit = payload.get("limit", 100)
    if not isinstance(source, str) or not source.strip() or len(source) > 8_000:
        return jsonify(error="Enter a SELECT query of up to 8,000 characters."), 400
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 200:
        return jsonify(error="The result limit must be between 1 and 200."), 400

    # SQL is user-authored code; data values and the result cap stay bound.
    statement = sql.SQL("SELECT * FROM ({}) AS query_result LIMIT %s").format(
        sql.SQL(source.strip().removesuffix(";")))
    return respond(lambda connection: result_rows(connection, statement, (limit + 1,), limit))


def result_rows(connection, statement, parameters, limit):
    with connection.cursor() as cursor:
        cursor.execute(statement, parameters)
        if len(cursor.description) > app.config["MAX_RESULT_COLUMNS"]:
            raise ResultTooLarge()
        columns = [column.name for column in cursor.description]
        records = cursor.fetchall()
    rows = [[cell(value) for value in row] for row in records[:limit]]
    body = {"columns": columns, "rows": rows, "row_count": min(len(records), limit),
            "truncated": len(records) > limit, "read_only": True}
    if len(json.dumps(body, ensure_ascii=False).encode()) > app.config["MAX_RESULT_BYTES"]:
        raise ResultTooLarge()
    return body


class ResultTooLarge(Exception):
    pass


class TableNotFound(Exception):
    pass


def respond(callback):
    connection = None
    pool = None
    timer = None
    discard_connection = False
    expired = threading.Event()
    started = time.monotonic()
    try:
        pool = connection_pool()
        connection = pool.getconn()

        def cancel_query():
            expired.set()
            try:
                connection.cancel_safe(timeout=2)
            except psycopg.Error:
                pass

        timer = threading.Timer(app.config["QUERY_TIMEOUT_SECONDS"], cancel_query)
        timer.daemon = True
        timer.start()
        body = callback(connection)
        if len(json.dumps(body, ensure_ascii=False).encode()) > app.config["MAX_RESULT_BYTES"]:
            raise ResultTooLarge()
        if expired.is_set():
            return jsonify(error="The query reached its time limit."), 408
        elapsed = round((time.monotonic() - started) * 1_000, 2)
        body.update(elapsed_ms=elapsed, read_only=True)
        if "preview" in body:
            body["preview"]["elapsed_ms"] = elapsed
        return jsonify(body)
    except ResultTooLarge:
        return jsonify(error="The result is too wide. Select fewer columns or smaller values."), 400
    except TableNotFound:
        return jsonify(error="This table is not available in the published catalog."), 404
    except (PoolTimeout, TooManyRequests, PoolClosed):
        return jsonify(error="The database is busy. Try again shortly."), 503
    except psycopg.Error as error:
        code = error.sqlstate
        if expired.is_set() or code == "57014":
            discard_connection = True
            return jsonify(error="The query reached its time limit.", sqlstate=code), 408
        if code == "42501":
            return jsonify(error="This account can only read the permitted database tables.", sqlstate=code), 403
        if code in {"53300", "53200"}:
            return jsonify(error="The database is busy. Try again shortly.", sqlstate=code), 503
        if not code or code.startswith("08"):
            return jsonify(error="The database connection is unavailable."), 503
        return jsonify(error="Check your SELECT query and its table or column names.", sqlstate=code), 400
    except (RuntimeError, ValueError, OSError):
        return jsonify(error="The database connection is not configured."), 503
    finally:
        if timer:
            timer.cancel()
            timer.join()  # cancel_safe has its own bound; no late cancellation may reach another request.
        if connection is not None:
            try:
                if discard_connection or expired.is_set() or connection.closed or connection.broken:
                    connection.close()
                else:
                    connection.rollback()
            except psycopg.Error:
                connection.close()
            finally:
                pool.putconn(connection)


@app.get("/tables")
def tables():
    if request.args:
        return jsonify(error="The catalog endpoint does not accept connection settings."), 400

    def catalog(connection):
        records = connection.execute(
            "SELECT table_schema,table_name,table_type FROM information_schema.tables "
            "WHERE table_catalog=current_database() AND table_schema NOT IN ('information_schema','pg_catalog') "
            "ORDER BY table_schema,table_name LIMIT %s", (101,)).fetchall()
        return {"tables": [{"schema": schema, "name": name, "type": kind}
                           for schema, name, kind in records[:100]], "truncated": len(records) > 100}

    return respond(catalog)


@app.get("/table")
def table():
    if set(request.args) != {"schema", "name"} or any(len(request.args.getlist(key)) != 1 for key in request.args):
        return jsonify(error="Choose one schema and table from the catalog."), 400
    schema, name = request.args["schema"], request.args["name"]
    if any(not value or len(value) > 128 or any(ord(character) < 32 for character in value) for value in (schema, name)):
        return jsonify(error="Choose a valid schema and table name."), 400

    def details(connection):
        records = connection.execute(
            "SELECT column_name,data_type,is_nullable FROM information_schema.columns "
            "WHERE table_catalog=current_database() AND table_schema=%s AND table_name=%s "
            "ORDER BY ordinal_position LIMIT %s", (schema, name, app.config["MAX_RESULT_COLUMNS"] + 1)).fetchall()
        if not records:
            raise TableNotFound()
        if len(records) > app.config["MAX_RESULT_COLUMNS"]:
            raise ResultTooLarge()
        statement = sql.SQL("SELECT * FROM {} LIMIT %s").format(sql.Identifier(schema, name))
        preview = result_rows(connection, statement, (11,), 10)
        return {"schema": schema, "name": name,
                "columns": [{"name": column, "type": kind, "nullable": nullable == "YES"}
                            for column, kind, nullable in records], "preview": preview}

    return respond(details)
