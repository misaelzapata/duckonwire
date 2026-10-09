import json
import os
from decimal import Decimal

import psycopg

ssl_mode = os.getenv("PGSSLMODE", "disable")
if ssl_mode not in {"disable", "verify-full"}:
    raise ValueError("Use PGSSLMODE=disable or verify-full")
if ssl_mode == "verify-full" and not os.getenv("PGSSLROOTCERT"):
    raise ValueError("Set PGSSLROOTCERT for verify-full")
owner = "PostgreSQL Python"
with psycopg.connect(
    host=os.getenv("PGHOST", "127.0.0.1"), port=os.getenv("PGPORT", "9496"),
    user=os.getenv("PGUSER", "duckonwire"), dbname=os.getenv("PGDATABASE", "duckonwire"),
    password=os.getenv("PGPASSWORD") or os.environ["DUCKONWIRE_TOKEN"],
    sslmode=ssl_mode, autocommit=True,
) as connection:
    connection.execute("CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))")
    with connection.transaction():
        connection.execute("DELETE FROM pg_demo_products WHERE client=%s", [owner])
        for row in [(1, "Coffee", Decimal("19.99"), 3), (2, "Rice", Decimal("8.50"), 10)]:
            connection.execute("INSERT INTO pg_demo_products VALUES(%s,%s,%s,%s,%s)", [owner, *row], prepare=True)
        connection.execute("UPDATE pg_demo_products SET stock=stock+1 WHERE client=%s AND id=%s", [owner, 1])
        connection.execute("DELETE FROM pg_demo_products WHERE client=%s AND id=%s", [owner, 2])
    selected = connection.execute("SELECT id,name,price,stock FROM pg_demo_products WHERE client=%s ORDER BY id", [owner]).fetchall()
    rows = [[row[0], row[1], str(row[2]), row[3]] for row in selected]
    assert rows == [[1, "Coffee", "19.99", 4]], rows
    print(json.dumps({"passed": True, "client": owner, "driver": "psycopg", "rows": rows}))
