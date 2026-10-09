// Ordinary pg clients send every id/product_id/amount value over the wire.
// Four writers commit 1,000-row prepared VALUES batches; four readers run concurrently.
import { Client } from 'pg';
import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

const sslMode = process.env.PGSSLMODE ?? 'disable';
if (!['disable', 'verify-full'].includes(sslMode)) throw new Error('Use PGSSLMODE=disable or verify-full');
if (sslMode === 'verify-full' && !process.env.PGSSLROOTCERT) throw new Error('Set PGSSLROOTCERT for verify-full');
const ssl = sslMode === 'verify-full' ? { ca: readFileSync(process.env.PGSSLROOTCERT), rejectUnauthorized: true } : false;

const rows = Number(process.env.PG_INGEST_ROWS ?? 2_000_000);
const batch = 1_000;
if (!Number.isSafeInteger(rows) || rows < 4_000 || rows % (4 * batch) !== 0) throw new Error('PG_INGEST_ROWS must be a positive multiple of 4000');
const insertSql = 'INSERT INTO pg_client_ingest_prices(id,product_id,amount) VALUES ' + Array.from({ length: batch }, (_, row) => '(' + [1, 2, 3].map(column => '$' + (row * 3 + column)).join(',') + ')').join(',');
const config = { host: process.env.PGHOST ?? '127.0.0.1', port: Number(process.env.PGPORT ?? 9496),
  user: process.env.PGUSER ?? 'duckonwire', database: process.env.PGDATABASE ?? 'duckonwire',
  password: process.env.PGPASSWORD ?? process.env.DUCKONWIRE_TOKEN, ssl };
const owner = new Client(config);
const writers = [owner, ...Array.from({ length: 3 }, () => new Client(config))];
if (Boolean(process.env.PGREADER_USER) !== Boolean(process.env.PGREADER_PASSWORD)) throw new Error('Set PGREADER_USER and PGREADER_PASSWORD together');
const readerConfig = process.env.PGREADER_USER ? { ...config, user: process.env.PGREADER_USER, password: process.env.PGREADER_PASSWORD } : config;
const readers = Array.from({ length: 4 }, () => new Client(readerConfig));
let createdProducts = false, createdPrices = false;
try {
  await owner.connect();
  await owner.query('CREATE TABLE pg_client_ingest_products(id INTEGER PRIMARY KEY,category INTEGER)');
  createdProducts = true;
  await owner.query('INSERT INTO pg_client_ingest_products SELECT i::INTEGER,(i%20)::INTEGER FROM range(20000) t(i)');
  await owner.query('CREATE TABLE pg_client_ingest_prices(id BIGINT PRIMARY KEY,product_id INTEGER,amount BIGINT)');
  createdPrices = true;
  const engine = (await owner.query('SELECT library_version,source_id FROM pragma_version()')).rows[0];
  const engineConfig = (await owner.query("SELECT current_setting('threads') AS threads,current_setting('memory_limit') AS memory_limit")).rows[0];
  await Promise.all([...writers.slice(1), ...readers].map(client => client.connect()));
  let pendingWriters = 4;
  let finishedWriting;
  const readSamples = [];
  const workload = [
    'SELECT p.id,p.amount,b.category FROM pg_client_ingest_prices p JOIN pg_client_ingest_products b ON p.product_id=b.id WHERE p.id=$1',
    'SELECT b.category,count(*) AS n,sum(p.amount) AS total FROM pg_client_ingest_prices p JOIN pg_client_ingest_products b ON p.product_id=b.id GROUP BY b.category',
    'WITH newest AS (SELECT product_id,amount,ROW_NUMBER() OVER(PARTITION BY product_id ORDER BY id DESC) r FROM pg_client_ingest_prices WHERE product_id<256) SELECT count(*) AS n,sum(amount) AS total FROM newest WHERE r=1',
    'SELECT count(*),sum(id),sum(amount) FROM pg_client_ingest_prices',
  ];
  const started = performance.now();
  const readTasks = readers.map(async (client, index) => {
    let query = index, overlap = 0, count = 0;
    do {
      const selected = query++ % workload.length;
      const start = performance.now();
      await client.query(workload[selected], selected === 0 ? [Math.floor(rows / 2)] : undefined);
      readSamples.push(performance.now() - start); count++;
      if (pendingWriters > 0) overlap++;
    } while (pendingWriters > 0);
    return { queries: count, overlappingQueries: overlap };
  });
  const writeTasks = writers.map(async (client, index) => {
    const end = Math.floor(rows * (index + 1) / 4);
    let commits = 0;
    try {
      for (let start = Math.floor(rows * index / 4); start < end; start += batch) {
        const values = [];
        for (let id = start; id < start + batch; id++) values.push(id, id % 20_000, id * 3);
        await client.query('BEGIN');
        const inserted = await client.query({ name: 'ingest_values_1000', text: insertSql, values });
        if (inserted.rowCount !== batch) throw new Error('Prepared VALUES affected-row count mismatch');
        await client.query('COMMIT'); commits++;
        if (process.env.PG_INGEST_PROGRESS === '1' && commits % 100 === 0) process.stderr.write(JSON.stringify({ writer: index, committedRows: commits * batch }) + '\n');
      }
      return { commits };
    } finally { if (--pendingWriters === 0) finishedWriting = performance.now(); }
  });
  const outcomes = await Promise.allSettled([...writeTasks, ...readTasks]);
  const failure = outcomes.find(result => result.status === 'rejected');
  if (failure) throw failure.reason;
  const written = outcomes.slice(0, 4).map(result => result.value);
  const read = outcomes.slice(4).map(result => result.value);
  const writeSeconds = (finishedWriting - started) / 1000;
  const integrity = (await owner.query('SELECT count(*) n,count(DISTINCT id) distinct_ids,min(id) minimum,max(id) maximum,sum(id) ids,sum(amount) amounts FROM pg_client_ingest_prices')).rows[0];
  const total = BigInt(rows), checksum = total * (total - 1n) / 2n;
  if (BigInt(integrity.n) !== total || BigInt(integrity.distinct_ids) !== total || BigInt(integrity.minimum) !== 0n || BigInt(integrity.maximum) !== total - 1n || BigInt(integrity.ids) !== checksum || BigInt(integrity.amounts) !== checksum * 3n) throw new Error('Integrity check failed');
  const grouped = (await owner.query(workload[1] + ' ORDER BY b.category')).rows;
  if (grouped.length !== 20 || !grouped.every((row, category) => {
    const count = (total - 1n - BigInt(category)) / 20n + 1n;
    const amount = 3n * (count * BigInt(category) + 20n * count * (count - 1n) / 2n);
    return row.category === category && BigInt(row.n) === count && BigInt(row.total) === amount;
  })) throw new Error('Grouped join result mismatch');
  const windowed = (await owner.query(workload[2])).rows[0];
  let windowAmount = 0n;
  for (let product = 0n; product < 256n; product++) windowAmount += 3n * (product + (total - 1n - product) / 20_000n * 20_000n);
  if (BigInt(windowed.n) !== 256n || BigInt(windowed.total) !== windowAmount) throw new Error('Window CTE result mismatch');
  if (!read.every(reader => reader.overlappingQueries > 0)) throw new Error('A reader did not complete during writes');
  readSamples.sort((a, b) => a - b);
  const rank = fraction => readSamples[Math.max(0, Math.ceil(readSamples.length * fraction) - 1)];
  console.log(JSON.stringify({ passed: true, driver: 'pg', rows, writers: 4, readers: 4,
    commits: written.reduce((sum, writer) => sum + writer.commits, 0), batchRows: batch, boundParametersPerBatch: batch * 3, boundParametersSent: rows * 3, writeSeconds, separateReadOnlyRole: Boolean(process.env.PGREADER_USER),
    rowsPerSecond: rows / writeSeconds, readerProgress: read, integrity, complexResultsVerified: true, engine, engineConfig,
    readLatency: { samples: readSamples.length, meanMs: readSamples.reduce((a, b) => a + b) / readSamples.length, p50Ms: rank(.5), p95Ms: rank(.95) },
    scope: 'One stock Node process and eight independent network connections. Timing includes client numeric row construction, bound VALUES transport and confirmed commits; authentication, table creation and final integrity checks excluded. In-session integrity; no server restart.' }));
} finally {
  await Promise.allSettled([...writers.slice(1), ...readers].map(client => client.end()));
  try {
    await owner.query('ROLLBACK').catch(() => {});
    if (createdPrices) await owner.query('DROP TABLE pg_client_ingest_prices');
    if (createdProducts) await owner.query('DROP TABLE pg_client_ingest_products');
  } finally {
    await owner.end();
  }
}
