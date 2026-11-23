import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { init, compileCharts } from '@tinyactors/wasm/init';
import { charts, workPacketXSD } from '../src/core/charts.mjs';

const root = new URL('../', import.meta.url);

test('src/core/charts.mjs holds the charts and the schema as they are on disk (mise run charts)', () => {
  const files = readdirSync(new URL('charts/', root)).filter((f) => f.endsWith('.scxml')).sort();
  assert.deepEqual(Object.keys(charts).sort(), files);
  for (const f of files) assert.equal(charts[f], readFileSync(new URL(`charts/${f}`, root), 'utf8'), `${f} is stale: run mise run charts`);
  assert.equal(workPacketXSD, readFileSync(new URL('spec/work-packet.xsd', root), 'utf8'));
});

test('the charts compile with no warning', async () => {
  await init();
  const compiled = compileCharts(Object.entries(charts).map(([name, text]) => ({ name, text })), { warningsAsErrors: true });
  assert.deepEqual(compiled.diagnostics, []);
  compiled.close();
});
