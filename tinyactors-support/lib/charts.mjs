// The charts, compiled once per process. Each chart reads at most one
// helper file with <script src>; the resolver puts lib.js, the helpers
// every chart shares, in front of it, since on the native datamodel a
// helper sees the helpers of its own script only. Warnings are errors: a
// syntax error, a top-level declaration of a script.

import { readdirSync, readFileSync } from 'node:fs';
import { compileCharts } from '@tinyactors/node';

const CHARTS = new URL('../charts/', import.meta.url);

function read(name) {
  if (!/^[a-z]+\.js$/.test(name)) throw new Error(`no helper file ${name}`);
  return readFileSync(new URL(name, CHARTS), 'utf8');
}

export function compile() {
  const sources = readdirSync(CHARTS).filter((f) => f.endsWith('.scxml')).sort()
    .map((name) => ({ name, text: readFileSync(new URL(name, CHARTS), 'utf8') }));
  return compileCharts(sources, {
    warningsAsErrors: true,
    resolver: (_base, source) => source === 'lib.js' ? read('lib.js') : `${read('lib.js')}\n${read(source)}`,
  });
}
