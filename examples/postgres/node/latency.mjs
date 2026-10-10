// Measure complete query round trips with the ordinary pg driver.
// Run against the public demo, without another build or load test in progress.
import { Client } from 'pg';
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { arch, platform } from 'node:os';
import { performance } from 'node:perf_hooks';
import { setTimeout as pause } from 'node:timers/promises';

function integer(name, fallback, minimum, maximum) {
  const value = Number(process.env[name] ?? fallback);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return value;
}
const samples = integer('PG_LATENCY_SAMPLES', 30, 1, 1_000);
const warmups = integer('PG_LATENCY_WARMUPS', 3, 0, 100);
const waitMs = integer('PG_LATENCY_PAUSE_MS', 200, 0, 10_000);
const limit = integer('PG_LATENCY_LIMIT', 100, 1, 200);
const offset = integer('PG_LATENCY_OFFSET', 0, 0, 1_000_000);
const sslMode = process.env.PGSSLMODE ?? 'disable';
if (!['disable', 'verify-full'].includes(sslMode)) throw new Error('Use PGSSLMODE=disable or verify-full');
if (sslMode === 'verify-full' && !process.env.PGSSLROOTCERT) throw new Error('Set PGSSLROOTCERT for verify-full');
const ssl = sslMode === 'verify-full'
  ? { ca: readFileSync(process.env.PGSSLROOTCERT), rejectUnauthorized: true } : false;
const client = new Client({
  host: process.env.PGHOST ?? '127.0.0.1', port: Number(process.env.PGPORT ?? 9496),
  user: process.env.PGUSER ?? 'duckonwire', database: process.env.PGDATABASE ?? 'duckonwire',
  password: process.env.PGPASSWORD ?? process.env.DUCKONWIRE_TOKEN, ssl,
});
const directory = new URL('../queries/', import.meta.url);
const queries = readdirSync(directory).filter(name => name.endsWith('.sql')).sort();
const results = [];
try {
  await client.connect();
  const engine = (await client.query('SELECT library_version,source_id FROM pragma_version()')).rows[0];
  const engineConfig = (await client.query("SELECT current_setting('threads') AS threads,current_setting('memory_limit') AS memory_limit")).rows[0];
  const data = (await client.query('SELECT (SELECT count(*) FROM products) AS products,(SELECT count(*) FROM prices) AS prices')).rows[0];
  for (const filename of queries) {
    const source = readFileSync(new URL(filename, directory), 'utf8').trim().replace(/;$/, '');
    const text = `SELECT * FROM (${source}) AS query_result LIMIT $1 OFFSET $2`;
    const times = [];
    let checksum, rowCount;
    for (let iteration = 0; iteration < warmups + samples; iteration++) {
      const started = performance.now();
      const response = await client.query({ text, values: [limit + 1, offset], rowMode: 'array' });
      const elapsed = performance.now() - started;
      const digest = createHash('sha256').update(JSON.stringify({
        fields: response.fields.map(field => [field.name, field.dataTypeID]), rows: response.rows,
      })).digest('hex');
      if (checksum && checksum !== digest) throw new Error(`Results changed during ${filename}; stop concurrent writes for this benchmark`);
      checksum = digest;
      rowCount = response.rowCount;
      if (iteration >= warmups) times.push(elapsed);
      if (waitMs) await pause(waitMs);
    }
    times.sort((a, b) => a - b);
    const rank = fraction => times[Math.max(0, Math.ceil(times.length * fraction) - 1)];
    results.push({ query: filename, samples: times.length, returnedRows: rowCount,
      resultSha256: checksum, meanMs: times.reduce((sum, value) => sum + value, 0) / times.length,
      p50Ms: rank(.5), p95Ms: rank(.95), minimumMs: times[0], maximumMs: times.at(-1) });
  }
  console.log(JSON.stringify({ passed: true, driver: 'pg', engine, engineConfig, data,
    client: { node: process.version, platform: platform(), architecture: arch() },
    verifiedTLS: sslMode === 'verify-full', warmups, pauseMs: waitMs, limit, offset, results,
    scope: 'Complete client query round trips on one reused connection, including bound parameter transport and decoded rows. Connection setup, pauses and SHA-256 checks excluded. Reads are uncached at the application layer; engine buffers may be warm. No concurrent writes.' }));
} finally {
  await client.end();
}
