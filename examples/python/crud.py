"""Real DuckDB driver: create, insert, update, select, delete and commit."""
import json
import os
from decimal import Decimal

import duckdb

endpoint = os.environ.get('DUCKONWIRE_ENDPOINT', 'quack:127.0.0.1:9495')
token = os.environ['DUCKONWIRE_TOKEN']
def quote(value):
    return "'" + value.replace("'", "''") + "'"
con = duckdb.connect()  # HTTPFS and Quack are already built in.
try:
    con.execute('CONNECT ' + quote(endpoint) + ' (TOKEN ' + quote(token) + ')')
    con.execute('CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,'
                'price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))')
    con.begin()
    con.execute('DELETE FROM demo_products WHERE client=?', ['Python'])
    con.executemany('INSERT INTO demo_products VALUES (?,?,?,?,?)', [
        ('Python', 1, 'Cafe', Decimal('19.99'), 3),
        ('Python', 2, 'Arroz', Decimal('8.50'), 10),
    ])
    con.execute('UPDATE demo_products SET stock=stock+? WHERE client=? AND id=?', [1, 'Python', 1])
    before_delete = con.execute('SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id', ['Python']).fetchall()
    assert before_delete == [(1, 'Cafe', Decimal('19.99'), 4), (2, 'Arroz', Decimal('8.50'), 10)]
    con.execute('DELETE FROM demo_products WHERE client=? AND id=?', ['Python', 2])
    con.commit()
    rows = con.execute('SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id', ['Python']).fetchall()
    assert rows == [(1, 'Cafe', Decimal('19.99'), 4)]
    print(json.dumps({'passed': True, 'language': 'Python', 'rows': rows}, default=str))
finally:
    con.close()
