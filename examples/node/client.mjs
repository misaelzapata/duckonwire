import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {performance} from 'node:perf_hooks';
import {fileURLToPath} from 'node:url';

const [major, minor] = process.versions.node.split('.').map(Number);
assert.ok(major > 24 || (major === 24 && minor >= 9), 'Use Node.js 24.9 or newer');
const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(process.argv[2] || path.join(directory, '../..'));
const state = JSON.parse(fs.readFileSync(process.argv[3] || path.join(root, '.runtime/native.json'), 'utf8'));
const runtime = [process.env.DUCKONWIRE_NODE_RUNTIME, path.join(root, 'drivers/node'), directory]
  .filter(Boolean).map(candidate => path.resolve(candidate))
  .find(candidate => fs.existsSync(path.join(candidate, 'node_modules/@duckdb/node-api/package.json')));
assert.ok(runtime, 'Provide @duckdb/node-api linked to native DuckDB v2.1.0-dev0 via '
  + 'DUCKONWIRE_NODE_RUNTIME, drivers/node, or examples/node/node_modules');
const require = createRequire(path.join(runtime, 'package.json'));
assert.equal(require('@duckdb/node-bindings').library_version(), 'v2.1.0-dev0',
  'The official Node binding must use the paired native DuckDB v2.1.0-dev0 engine');
const {DuckDBInstance, BIGINT, VARCHAR, DECIMAL, BLOB, INTEGER, LIST, STRUCT, StatementType, ResultReturnType,
  decimalValue, blobValue, listValue, structValue} = require('@duckdb/node-api');
const literal = value => `'${value.replaceAll("'", "''")}'`;
const started = performance.now();
const report = {language: 'Node.js', driver: '@duckdb/node-api', driver_version: '1.5.6-r.1',
  node_version: process.version, native_used: true, custom_protocol_wrapper: false, passed: false, checks: []};
const instance = await DuckDBInstance.create(':memory:', {threads: '2'});
const connections = [];
const check = async (name, action) => {
  const begin = performance.now();
  await action();
  report.checks.push({name, passed: true, seconds: (performance.now() - begin) / 1000});
};
const rows = async (con, sql, values, types) => (await con.runAndReadAll(sql, values, types)).getRows();
const count = async con => (await rows(con, 'SELECT count(*) FROM node_native_probe'))[0][0];
const connect = async () => {
  const con = await instance.connect();
  connections.push(con);
  await con.run(`CONNECT ${literal(state.endpoint)} (TOKEN ${literal(state.token)})`);
  return con;
};

try {
  const a = await connect();
  const b = await connect();
  await check('official_native_driver_and_matching_engine', async () => {
    report.engine_version = (await rows(a, 'SELECT version()'))[0][0];
    assert.equal(report.engine_version, 'v2.1.0-dev0');
    assert.equal(require('@duckdb/node-bindings').library_version(), report.engine_version);
    const local = await instance.connect();
    try {
      report.builtin_extensions = await rows(local,
        `SELECT extension_name,loaded,installed,install_mode FROM duckdb_extensions()
         WHERE extension_name IN ('httpfs','quack') ORDER BY extension_name`);
      assert.deepEqual(report.builtin_extensions, [
        ['httpfs', true, true, 'STATICALLY_LINKED'], ['quack', true, true, 'STATICALLY_LINKED'],
      ]);
      assert.deepEqual(await rows(local, `SELECT current_setting('allow_unsigned_extensions')`), [[false]]);
      report.manual_load_used = false;
      report.allow_unsigned_extensions = false;
    } finally {local.closeSync();}
  });
  await a.run('DROP TABLE IF EXISTS node_native_probe');
  await a.run(`CREATE TABLE node_native_probe(id BIGINT PRIMARY KEY,label VARCHAR,amount DECIMAL(18,4),
    payload BLOB,items INTEGER[],attrs STRUCT(note VARCHAR,n INTEGER),nullable INTEGER)`);
  const values = [1n, "Node café ' ? $name", decimalValue(123456789n, 18, 4),
    blobValue(Uint8Array.from([0, 127, 128, 255])), listValue([3, null, 5]),
    structValue({note: 'native λ', n: 7}), null];
  const types = [BIGINT, VARCHAR, DECIMAL(18, 4), BLOB, LIST(INTEGER),
    STRUCT({note: VARCHAR, n: INTEGER}), INTEGER];
  const insert = await a.prepare('INSERT INTO node_native_probe VALUES (?,?,?,?,?,?,?)');
  await check('prepared_positional_decimal_blob_list_struct_null_and_unicode', async () => {
    assert.equal(insert.parameterCount, 7);
    insert.bind(values, types);
    const inserted = await insert.run();
    assert.equal(inserted.rowsChanged, 1);
    assert.equal(inserted.statementType, StatementType.INSERT);
    assert.equal(inserted.returnType, ResultReturnType.CHANGED_ROWS);
    const result = (await rows(b, 'SELECT * FROM node_native_probe'))[0];
    assert.equal(result[0], 1n);
    assert.equal(result[1], values[1]);
    assert.equal(result[2].value, 123456789n);
    assert.equal(result[2].width, 18);
    assert.equal(result[2].scale, 4);
    assert.deepEqual([...result[3].bytes], [0, 127, 128, 255]);
    assert.deepEqual(result[4].items, [3, null, 5]);
    assert.deepEqual(result[5].entries, {note: 'native λ', n: 7});
    assert.equal(result[6], null);
  });
  await check('fresh_named_parameters_and_prepared_reuse_null_to_integer', async () => {
    assert.deepEqual(await rows(a, 'SELECT $number::INTEGER AS first,$text::VARCHAR AS text,$number::INTEGER AS second',
      {number: 42, text: 'named λ'}, {number: INTEGER, text: VARCHAR}), [[42, 'named λ', 42]]);
    values[0] = 2n;
    values[6] = 91;
    insert.bind(values, types);
    await insert.run();
    assert.deepEqual(await rows(b, 'SELECT nullable FROM node_native_probe ORDER BY id'), [[null], [91]]);
  });
  await check('two_sessions_uncommitted_visibility_commit_and_rollback', async () => {
    await a.run('BEGIN');
    values[0] = 3n;
    insert.bind(values, types);
    await insert.run();
    assert.equal(await count(a), 3n);
    assert.equal(await count(b), 2n);
    await a.run('ROLLBACK');
    assert.equal(await count(b), 2n);
    await a.run('BEGIN');
    insert.bind(values, types);
    await insert.run();
    await a.run('COMMIT');
    assert.equal(await count(b), 3n);
  });
  await check('MVCC_reader_snapshot_progresses_while_other_session_writes', async () => {
    await b.run('BEGIN');
    assert.equal((await rows(b, 'SELECT label FROM node_native_probe WHERE id=1'))[0][0], values[1]);
    await a.run('UPDATE node_native_probe SET label=$label WHERE id=$id',
      {label: 'updated from Node', id: 1n}, {label: VARCHAR, id: BIGINT});
    assert.equal((await rows(b, 'SELECT label FROM node_native_probe WHERE id=1'))[0][0], values[1]);
    await b.run('ROLLBACK');
    assert.equal((await rows(b, 'SELECT label FROM node_native_probe WHERE id=1'))[0][0], 'updated from Node');
  });
  await check('many_commits_progress_while_reader_holds_snapshot', async () => {
    await b.run('BEGIN');
    assert.equal(await count(b), 3n);
    let finished = false;
    let reads = 0;
    const writer = (async () => {
      try {
        for (let id = 100n; id < 164n; id++) {
          const row = [...values];
          row[0] = id;
          insert.bind(row, types);
          assert.equal((await insert.run()).rowsChanged, 1);
        }
      } finally {finished = true;}
    })();
    const reader = (async () => {
      while (!finished) {
        assert.equal(await count(b), 3n);
        reads++;
      }
    })();
    await Promise.all([writer, reader]);
    assert.ok(reads > 0);
    await b.run('ROLLBACK');
    assert.equal(await count(b), 67n);
    assert.equal((await a.run('DELETE FROM node_native_probe WHERE id>=100')).rowsChanged, 64);
    report.concurrent_commits = 64;
    report.concurrent_snapshot_reads = reads;
  });
  await check('duplicate_result_names_and_DML_RETURNING_keep_native_metadata', async () => {
    const duplicate = await a.runAndReadAll('SELECT $n::INTEGER,$n::INTEGER', {n: 9}, {n: INTEGER});
    assert.deepEqual(duplicate.getRows(), [[9, 9]]);
    assert.equal(duplicate.columnName(0), duplicate.columnName(1));
    const returning = await a.run('UPDATE node_native_probe SET nullable=nullable WHERE id=1 RETURNING id');
    assert.equal(returning.statementType, StatementType.UPDATE);
    assert.equal(returning.returnType, ResultReturnType.QUERY_RESULT);
    assert.deepEqual(await returning.getRows(), [[1n]]);
  });
  await check('native_constraint_error_and_connection_reuse', async () => {
    insert.bind(values, types);
    await assert.rejects(insert.run(), /Constraint Error|PRIMARY KEY|duplicate/i);
    assert.deepEqual(await rows(a, 'SELECT 42'), [[42]]);
    await assert.rejects(a.run('SELECT $one,$two', {one: 1}, {one: INTEGER}), /missing|provided|mismatch/i);
    assert.deepEqual(await rows(a, 'SELECT 43'), [[43]]);
  });
  await check('native_stream_paging_with_unrelated_writer', async () => {
    const stream = await b.stream('SELECT i::BIGINT FROM range(15003) AS r(i)');
    let observed = 0;
    let chunks = 0;
    const write = a.run('UPDATE node_native_probe SET nullable=nullable+1 WHERE id=2');
    for (;;) {
      const chunk = await stream.fetchChunk();
      if (!chunk || chunk.rowCount === 0) break;
      chunks++;
      for (const row of chunk.getRows()) assert.equal(row[0], BigInt(observed++));
    }
    await write;
    assert.equal(observed, 15003);
    assert.ok(chunks > 1);
    report.stream_rows = observed;
    report.stream_chunks = chunks;
  });
  await check('native_disconnect_rolls_back_abandoned_transaction', async () => {
    const abandoned = await connect();
    await abandoned.run('BEGIN');
    await abandoned.run('UPDATE node_native_probe SET label=? WHERE id=?', ['abandoned', 1n], [VARCHAR, BIGINT]);
    abandoned.closeSync();
    connections.splice(connections.indexOf(abandoned), 1);
    assert.equal((await rows(b, 'SELECT label FROM node_native_probe WHERE id=1'))[0][0], 'updated from Node');
    await a.run('UPDATE node_native_probe SET label=? WHERE id=?', ['updated from Node', 1n], [VARCHAR, BIGINT]);
  });
  await check('native_delete_commit_is_visible_and_shared_language_marker', async () => {
    await a.run('DELETE FROM node_native_probe WHERE id IN (2,3)');
    assert.equal(await count(b), 1n);
    const shared = await rows(a, `SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'`);
    if (shared[0][0] > 0n) {
      assert.deepEqual(await rows(a, 'SELECT label FROM lang_multiclient_probe WHERE id=0'), [['from Python']]);
      await a.run('INSERT INTO lang_multiclient_probe(id,label) VALUES (?,?)',
        [3n, 'from Node.js'], [BIGINT, VARCHAR]);
      report.shared_language_marker = true;
    }
  });
  insert.destroySync();
  report.passed = true;
} catch (error) {
  report.error = String(error.message).replaceAll(state.token, '[REDACTED]');
  report.failed_check_after = report.checks.at(-1)?.name || 'startup';
  process.exitCode = 1;
} finally {
  for (const con of connections.reverse()) {
    try {con.closeSync();} catch {}
  }
  instance.closeSync();
  report.seconds = (performance.now() - started) / 1000;
  console.log(JSON.stringify(report));
}
