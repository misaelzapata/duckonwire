"""Exercise the ordinary Flask client against an existing protected reader account."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import psycopg

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples" / "postgres" / "flask"
sys.path.insert(0, str(EXAMPLE))
from app import PRESETS, app, connection_settings  # noqa: E402

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
    client = app.test_client()
    page = client.get("/")
    check("HTML page renders without backend credentials", page.status_code == 200
          and os.environ["PGPASSWORD"].encode() not in page.data
          and b"Query the catalog." in page.data)
    for preset in PRESETS:
        response = client.post("/query", json={"sql": preset["sql"], "limit": 5})
        body = response.get_json()
        check("actual catalog preset " + preset["id"], response.status_code == 200
              and body["row_count"] == 5 and body["truncated"] and body["read_only"])
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
    original_timeout = app.config["QUERY_TIMEOUT_SECONDS"]
    try:
        app.config["QUERY_TIMEOUT_SECONDS"] = 0.15
        before = time.monotonic()
        response = client.post("/query", json={"sql": "SELECT sum(a.i*b.i) "
                              "FROM range(100000000) a(i), range(100000000) b(i)"})
        check("expensive query is canceled and request releases its connection",
              response.status_code == 408 and time.monotonic() - before < 5)
    finally:
        app.config["QUERY_TIMEOUT_SECONDS"] = original_timeout
    response = client.post("/query", json={"sql": "SELECT count(*) FROM products"})
    check("connection reusable and catalog unchanged after errors and cancellation",
          response.status_code == 200 and response.get_json()["rows"] == [[46462]])


if __name__ == "__main__":
    result = {"scope": "Ordinary Flask/psycopg client against an existing TLS/SCRAM reader role; "
              "no server orchestration or engine build", "driver": "psycopg",
              "driver_version": psycopg.__version__, "checks": checks}
    try:
        run()
        result.update(passed=True, native_reader_verified=True, rows_limit=200,
                      source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted(EXAMPLE.rglob("*"))
                                     if p.is_file() and "__pycache__" not in p.parts})
    except Exception as error:
        result.update(passed=False, error_type=type(error).__name__)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["passed"] else 1)
