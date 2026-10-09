import { Client } from 'pg';
import { readFileSync } from 'node:fs';

const sslMode = process.env.PGSSLMODE ?? 'disable';
if (!['disable', 'verify-full'].includes(sslMode)) throw new Error('Use PGSSLMODE=disable or verify-full');
if (sslMode === 'verify-full' && !process.env.PGSSLROOTCERT) throw new Error('Set PGSSLROOTCERT for verify-full');
const ssl = sslMode === 'verify-full' ? { ca: readFileSync(process.env.PGSSLROOTCERT), rejectUnauthorized: true } : false;

const client = new Client({
  host: process.env.PGHOST ?? '127.0.0.1', port: Number(process.env.PGPORT ?? 9496),
  user: process.env.PGUSER ?? 'duckonwire', database: process.env.PGDATABASE ?? 'duckonwire',
  password: process.env.PGPASSWORD ?? process.env.DUCKONWIRE_TOKEN, ssl,
});
const owner = 'PostgreSQL Node';
try {
  await client.connect();
  await client.query('CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))');
  await client.query('BEGIN');
  await client.query('DELETE FROM pg_demo_products WHERE client=$1', [owner]);
  for (const row of [[1, 'Coffee', '19.99', 3], [2, 'Rice', '8.50', 10]]) {
    await client.query({ name: 'insert_product', text: 'INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)', values: [owner, ...row] });
  }
  await client.query('UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2', [owner, 1]);
  await client.query('DELETE FROM pg_demo_products WHERE client=$1 AND id=$2', [owner, 2]);
  await client.query('COMMIT');
  const result = await client.query('SELECT id,name,price,stock FROM pg_demo_products WHERE client=$1 ORDER BY id', [owner]);
  const rows = result.rows.map(row => [row.id, row.name, row.price, row.stock]);
  if (JSON.stringify(rows) !== JSON.stringify([[1, 'Coffee', '19.99', 4]])) throw new Error('Unexpected committed row');
  console.log(JSON.stringify({ passed: true, client: owner, driver: 'pg', rows }));
} finally {
  await client.end();
}
