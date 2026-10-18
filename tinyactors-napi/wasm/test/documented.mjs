// The user documentation's examples that do not run in Chromium against
// @tinyactors/wasm (wasm/test/documentation.mjs), each with its reason:
// a ```js block of README.md or docs/guide/*.md by its document and line,
// as test/docs.test.mjs names it, and an example application
// (examples/<name>/) whole or by the cases of its test.mjs.
//
// A block is found to need Node.js by what it uses (`needs`): a `node:`
// module, `process`, `Buffer`, `setImmediate`, or a quickjs host. One that
// uses any of them must be listed here, and one listed here must use one,
// so the list stays true. A block marked ```js browser runs in Chromium
// only (tools/examples.mjs).

/** What `program` uses that a page does not have: each a reason's key. */
export function needs(program) {
  const found = new Set();
  for (const [, name] of program.matchAll(/(?:from |import\()'node:([\w/]+)'/g)) found.add(`node:${name}`);
  if (/\bprocess\.\w/.test(program)) found.add('process');
  if (/\bBuffer\b/.test(program)) found.add('Buffer');
  if (/\bsetImmediate\b/.test(program)) found.add('setImmediate');
  // a quickjs host of @tinyactors/node (one of @tinyactors/wasm is refused, which a page shows)
  if (/datamodel: 'quickjs'/.test(program) && !/'@tinyactors\/wasm/.test(program)) found.add('quickjs');
  return [...found].sort();
}

// document:line -> why the block stays in node and bun
export const BLOCKS = {
  'docs/guide/charts.md:382': 'directoryResolver reads charts from a temporary directory it writes with node:fs, node:os and node:path',
  'docs/guide/other-systems.md:112': 'two systems over a node:net socket, framed with Buffer',
  'docs/guide/quickjs.md:19': 'a quickjs host, which @tinyactors/wasm does not have until p5-quickjs',
  'docs/guide/testing.md:21': 'a test written with node:assert/strict, as the guide recommends for node and bun',
};

// The example applications that do not run in a page, whole.
export const APPLICATIONS = {
  'chat-rooms': 'its hibernated users live in files (node:fs/promises), and its demo runs as a process',
  'document-approval': 'its admin view is a node:http server, and it keeps its records in files (node:fs)',
  'http-gateway': 'it is a node:http server',
  'inventory-sync': 'its two systems are two processes (node:child_process) over a node:net socket',
  'iot-rules': 'its rules run in quickjs sandboxes (p5-quickjs), and it measures them in processes (node:child_process)',
  'remote-python': 'its other system is a Python process over a socket',
  'turn-game': 'its server is a process it kills and restores (node:child_process), its journal a file (node:fs)',
};

// name -> case -> reason, for an application that runs in a page; a case
// named "quickjs: ..." is skipped for quickjs without being listed.
export const CASES = {
  'elevator-control': {
    'the scenarios are the same for the same seed, and so is what both datamodels make of them': 'compares the native datamodel with quickjs (p5-quickjs)',
  },
  'job-scheduler': {
    'a process exits by itself when its last job ends, with its host still open': 'runs main.mjs as a process (node:child_process)',
    'SIGTERM: a graceful shutdown, what is dropped at close, and a clean exit': 'runs main.mjs as a process and signals it (node:child_process)',
    'a handler\'s error with no idle() waiting is an uncaught exception that ends the process': 'runs a program as a process (node:child_process)',
  },
  'order-fulfillment': {
    'the demo on the real clock: every order ends, and the ledger reconciles': 'writes its outbox into a temporary directory (node:fs, node:os)',
  },
  'subscription-billing': {},
};
