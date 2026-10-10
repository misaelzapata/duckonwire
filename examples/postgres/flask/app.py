"""Read-only Flask example. Configure a protected reader account using PG* env vars."""
import datetime
import decimal
import math
import os
import threading
import time
import uuid

from flask import Flask, jsonify, render_template, request
import psycopg
from psycopg import sql

app = Flask(__name__)
app.config.update(MAX_CONTENT_LENGTH=16_384, QUERY_TIMEOUT_SECONDS=28)

PRESETS = [
    {"id": "products", "name": "Browse products", "hint": "Public brands from Argentina and Spain",
     "sql": "SELECT id, name, brand, country_code, size, unit\nFROM products\nORDER BY id"},
    {"id": "prices", "name": "See sample prices", "hint": "Generated prices, not retail offers",
     "sql": "SELECT p.name, p.brand, p.country_code, pr.price, pr.day\nFROM products p\nJOIN prices pr ON pr.product_id = p.id\nORDER BY p.id"},
    {"id": "brands", "name": "Compare brands", "hint": "A grouped join over the catalog",
     "sql": "SELECT p.country_code, p.brand, count(*) AS products,\n       round(avg(pr.price), 2) AS average_price\nFROM products p JOIN prices pr ON pr.product_id = p.id\nGROUP BY p.country_code, p.brand\nORDER BY products DESC, p.brand"},
    {"id": "window", "name": "Market comparison", "hint": "Window functions over sample prices",
     "sql": "SELECT p.name, p.brand, p.country_code, pr.price,\n       round(avg(pr.price) OVER (PARTITION BY p.country_code), 2) AS market_average\nFROM products p JOIN prices pr ON pr.product_id = p.id\nORDER BY p.id"},
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
        "default-src 'self'; script-src 'self'; style-src 'self'; "
        "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'"
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

    connection = None
    timer = None
    expired = threading.Event()
    started = time.monotonic()
    try:
        connection = psycopg.connect(**connection_settings())

        def cancel_query():
            expired.set()
            try:
                connection.cancel_safe(timeout=2)
            except psycopg.Error:
                pass

        timer = threading.Timer(app.config["QUERY_TIMEOUT_SECONDS"], cancel_query)
        timer.daemon = True
        timer.start()
        # SQL is user-authored code; the row limit is a bound value. A derived
        # SELECT rejects standalone commands and caps rows at the database.
        statement = sql.SQL("SELECT * FROM ({}) AS query_result LIMIT %s").format(
            sql.SQL(source.strip().removesuffix(";")))
        with connection.cursor() as cursor:
            cursor.execute(statement, (limit + 1,))
            columns = [column.name for column in cursor.description]
            rows = cursor.fetchall()
        if expired.is_set():
            return jsonify(error="The query reached its time limit."), 408
        return jsonify(columns=columns, rows=[[cell(value) for value in row] for row in rows[:limit]],
                       row_count=min(len(rows), limit), truncated=len(rows) > limit,
                       elapsed_ms=round((time.monotonic() - started) * 1_000, 2), read_only=True)
    except psycopg.Error as error:
        code = error.sqlstate
        if expired.is_set() or code == "57014":
            return jsonify(error="The query reached its time limit.", sqlstate=code), 408
        if code == "42501":
            return jsonify(error="This account can only read the permitted database tables.", sqlstate=code), 403
        if code == "53300" or code == "53200":
            return jsonify(error="The database is busy. Try again shortly.", sqlstate=code), 503
        if not code or code.startswith("08"):
            return jsonify(error="The database connection is unavailable."), 503
        return jsonify(error="Check your SELECT query and its table or column names.", sqlstate=code), 400
    except (RuntimeError, ValueError, OSError):
        return jsonify(error="The database connection is not configured."), 503
    finally:
        if timer:
            timer.cancel()
            timer.join(timeout=2)
        if connection:
            try:
                connection.rollback()
            except psycopg.Error:
                pass
            finally:
                connection.close()
