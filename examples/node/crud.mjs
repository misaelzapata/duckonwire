import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';

// Use the official driver linked to the paired native DuckDB v2.1.0-dev0 engine.
const [major, minor] = process.versions.node.split('.').map(Number);
if (major < 24 || (major === 24 && minor < 9)) throw new Error('Use Node.js 24.9 or newer');
const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(directory, '../..');
const endpoint = process.env.DUCKONWIRE_ENDPOINT || 'quack:127.0.0.1:9495';
const token = process.env.DUCKONWIRE_TOKEN;
if (!token) throw new Error('Set DUCKONWIRE_TOKEN to the server token');
const runtime = [process.env.DUCKONWIRE_NODE_RUNTIME, path.join(root, 'drivers/node'), directory]
  .filter(Boolean).map(dir => path.resolve(dir))
  .find(dir => fs.existsSync(path.join(dir, 'node_modules/@duckdb/node-api/package.json')));
if (!runtime) throw new Error('Provide @duckdb/node-api linked to native DuckDB v2.1.0-dev0 via '
  + 'DUCKONWIRE_NODE_RUNTIME, drivers/node, or examples/node/node_modules');
const require = createRequire(path.join(runtime, 'package.json'));
if (require('@duckdb/node-bindings').library_version() !== 'v2.1.0-dev0') {
  throw new Error('The official Node binding must use the paired native DuckDB v2.1.0-dev0 engine');
}
const {DuckDBInstance, VARCHAR, INTEGER, DECIMAL, decimalValue} = require('@duckdb/node-api');
const literal = value => `'${value.replaceAll("'", "''")}'`;
const instance = await DuckDBInstance.create();
const con = await instance.connect();
try {
  // Quack and httpfs are already built into this native DuckDB engine.
  await con.run(`CONNECT ${literal(endpoint)} (TOKEN ${literal(token)})`);
  await con.run(`CREATE TABLE IF NOT EXISTS demo_products(
    client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,
    PRIMARY KEY(client,id))`);
  await con.run('BEGIN');
  await con.run('DELETE FROM demo_products WHERE client=?', ['Node.js'], [VARCHAR]);
  const insert = await con.prepare('INSERT INTO demo_products VALUES (?,?,?,?,?)');
  const types = [VARCHAR, INTEGER, VARCHAR, DECIMAL(18, 2), INTEGER];
  insert.bind(['Node.js', 1, 'Cafe', decimalValue(1999n, 18, 2), 3], types);
  await insert.run();
  insert.bind(['Node.js', 2, 'Arroz', decimalValue(850n, 18, 2), 10], types);
  await insert.run();
  insert.destroySync();
  await con.run('UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?',
    ['Node.js', 1], [VARCHAR, INTEGER]);
  await con.runAndReadAll('SELECT id,name,price,stock FROM demo_products WHERE client=? ORDER BY id',
    ['Node.js'], [VARCHAR]);
  await con.run('DELETE FROM demo_products WHERE client=? AND id=?', ['Node.js', 2], [VARCHAR, INTEGER]);
  await con.run('COMMIT');
  const result = await con.runAndReadAll(
    'SELECT id,name,price::VARCHAR,stock FROM demo_products WHERE client=? ORDER BY id',
    ['Node.js'], [VARCHAR]);
  console.log(JSON.stringify({passed: true, language: 'Node.js', rows: result.getRows()}));
} catch {
  // Error text is omitted because a CONNECT error can contain the private token.
  console.log(JSON.stringify({passed: false, language: 'Node.js', error: 'native CRUD failed'}));
  process.exitCode = 1;
} finally {
  con.closeSync();
}
