// The guest program each JavaScript engine runs for one case of
// `mise run bench-engines`: guest.js, the case's fixtures built by its
// generators, and the case's source as a function, behind one object,
// globalThis.__bench:
//
//   run(k)      calls the case k times inside the engine and returns the
//               sum of __fold over the results (so no result goes unused)
//   once()      one result
//   fixtures()  each global as __show prints it: { name: { len, hash } }
//   checkOnce() one result as __show prints it: { show, len, hash, fold }
//
// The cases come from bench.c (dump.c's `cases`, one JSON line each): their
// sources and literals are used as they are; a fixture is the generator of
// guest.js that mirrors bench.c's (FIXTURES below). A module (power.js,
// campaign.js) runs as a function body, since the guest is a script: its
// exports become consts and its import is dropped (campaign's `fill` is
// then an unresolved global, compiled but never called).
import { readFileSync } from 'node:fs';

const MODULES = new URL('../modules/', import.meta.url);
const LIBRARY = readFileSync(new URL('guest.js', import.meta.url), 'utf8');
const moduleBody = name => readFileSync(new URL(`${name}.js`, MODULES), 'utf8')
  .replace(/^import .*$/m, '')
  .replace(/^export /gm, '');

// bench.c's fixture (dump.c's name for it) -> a guest expression of N.
const FIXTURES = {
  literal: g => `(${g.literal})`,
  xs: () => '__makeXs(N)',
  rows: () => '__makeRows(N)',
  ids: () => '__makeIDs(N)',
  setA: () => '__makeStringSet(0, N)',
  setB: () => '__makeStringSet(Math.floor(N / 2), N)',
  pendingHalf: () => '__makePendingHalf(N)',
  numbers: () => '__numbersRaw(N)',
  text: () => '__makeText(N)',
  waterFill: () => '__power().waterFill',
  capacity: () => '(6 * N)',
  demand: () => '__makeDemand(N)',
  numberTexts15: () => '__makeNumberTexts(N, 15)',
  numberTexts17: () => '__makeNumberTexts(N, 17)',
  bigints: () => '__makeBigints(N)',
  idMap: () => '__makeIDMap(N)',
  rowsJSON: () => '__makeRowsJSON(N)',
};

// name -> the case as dump.c printed it.
export function loadCases(file) {
  return new Map(readFileSync(file, 'utf8').trim().split('\n').map(line => JSON.parse(line)).map(c => [c.name, c]));
}

// Why the case cannot run in the guest, or null.
export function unsupported(c) {
  if (c.op === 'compile') return ['power', 'campaign'].includes(c.text) ? null : 'only modules are compiled';
  if (c.op !== 'eval') return `op ${c.op} has no JavaScript counterpart`;
  if (c.text !== 'inline' && c.text !== 'power') return `text ${c.text} is not supported`;
  const missing = c.globals.find(g => !FIXTURES[g.fixture]);
  return missing ? `fixture ${missing.fixture ?? '(unnamed)'} of ${missing.name} has no generator in guest.js` : null;
}

export function guestSource(c, n) {
  const problem = unsupported(c);
  if (problem) throw new Error(`${c.name}: ${problem}`);
  let body;
  if (c.op === 'compile') {
    // A unique source each time, so no engine reuses a cached compilation.
    body = `(() => { const __src = ${JSON.stringify(moduleBody(c.text))}; let __i = 0; ` +
      'return () => new Function(__src + "\\n//" + (__i++)); })()';
  } else if (c.text === 'power') {
    body = '__power';
  } else {
    body = `() => (${c.source})`;
  }
  const globals = c.globals.map(g => `const ${g.name} = ${FIXTURES[g.fixture](g)};`).join('\n');
  const shown = c.globals.map(g =>
    `{ const s = __show(${g.name}); o[${JSON.stringify(g.name)}] = { len: s.length, hash: __hash(s) }; }`).join(' ');
  return `globalThis.__bench = (function () {
"use strict";
${LIBRARY}
function __power() {
${moduleBody('power')}
return { example, fill, waterFill };
}
const N = ${n ?? 1};
${globals}
const __f = ${body};
let __sink = 0;
return {
  run(k) { let c = 0; for (let i = 0; i < k; i++) c += __fold(__f()); __sink += c; return c; },
  once() { return __f(); },
  fixtures() { const o = {}; ${shown} return JSON.stringify(o); },
  checkOnce() {
    const v = __f(), s = __show(v);
    return JSON.stringify({ show: s.length <= 4000 ? s : null, len: s.length, hash: __hash(s), fold: __fold(v) });
  },
};
})();
`;
}
