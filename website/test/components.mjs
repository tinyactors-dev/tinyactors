// The trace view's pure parts, in Node.js (mise run test): the span tree,
// the time format and the axis. Its rendering runs in a browser only.
import assert from 'node:assert/strict';

globalThis.HTMLElement = class {};
globalThis.customElements = { define() {}, get() {} };
const { buildTree, flatten, formatTime, ticks } = await import('../components/trace-view.mjs');

const span = (id, parent, start, end = start + 1) => ({ span: id, parent, start, end, name: id, attributes: {}, events: [], status: 0 });

// Children follow their parent, sorted by start; several roots stay at the
// top; a span whose parent is missing hangs under one row per parent.
const roots = buildTree([
  span('late-root', null, 50),
  span('child-b', 'root', 20),
  span('root', null, 0),
  span('child-a', 'root', 10),
  span('grandchild', 'child-a', 11),
  span('orphan-1', 'gone', 30),
  span('orphan-2', 'gone', 40),
]);
assert.deepEqual(roots.map((node) => node.key), ['root', 'missing:gone', 'late-root']);
assert.deepEqual(roots[0].children.map((node) => node.key), ['child-a', 'child-b']);
assert.equal(roots[0].size, 3);
assert.deepEqual(roots[1].children.map((node) => node.key), ['orphan-1', 'orphan-2']);
assert.equal(roots[1].start, 30);
assert.equal(roots[1].end, 41);

// Collapsing a row hides its descendants, not its siblings.
const rows = (collapsed) => flatten(roots, new Set(collapsed)).map(({ node, depth }) => `${depth}:${node.key}`);
assert.deepEqual(rows([]), ['0:root', '1:child-a', '2:grandchild', '1:child-b', '0:missing:gone', '1:orphan-1', '1:orphan-2', '0:late-root']);
assert.deepEqual(rows(['child-a', 'missing:gone']), ['0:root', '1:child-a', '1:child-b', '0:missing:gone', '0:late-root']);

// Time reads in the unit a person would say.
assert.deepEqual([0, 850, 12e3, 3.4e6, 30e9, 90e9, 900e9].map(formatTime), ['0', '850 ns', '12 µs', '3.40 ms', '30 s', '90 s', '15 min']);
assert.deepEqual(ticks(120e9).map(formatTime), ['0', '25 s', '50 s', '75 s', '100 s']);

console.log('the trace view builds, folds and labels its tree');
