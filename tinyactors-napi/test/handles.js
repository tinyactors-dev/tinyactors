'use strict';

// One host expression per JavaScript value (napi ADR 0030, core ADR 0087):
// the same functions, locations and values are the same handles, so core
// answers the live definition for them; new closures register beside; a
// changed structure under the same revision is refused; a handle goes when
// the last definition using it is freed.

const assert = require('node:assert/strict');
const { createSystem, statechart, literal, TinyactorsError } = require('..');
const native = require('../lib/native');

const gc = globalThis.gc ?? (typeof Bun === 'undefined'
  ? (require('node:v8').setFlagsFromString('--expose-gc'), require('node:vm').runInNewContext('gc'))
  : () => Bun.gc(true));

function chart(guard, values = {}) {
  let builder = statechart({ family: 'handles', revision: '1' }).data('count', 0);
  for (const [name, value] of Object.entries(values)) {
    builder = builder.data(name, value);
  }
  return builder
    .initial('idle')
    .state('idle', (s) => s.on('go', (t) => t.when(guard).target('busy')))
    .state('busy');
}

// The same functions and values: one definition, with a second hold.
function sameHandles() {
  const system = createSystem();
  const guard = function () { return this.count > 0; };
  const first = system.define(chart(guard));
  const again = system.define(chart(guard));
  assert.equal(again.id, first.id);
  assert.equal(first.inspect().holds, 2);
  system.close();
}

// Code that makes its functions anew builds charts that register beside.
function newClosures() {
  const system = createSystem();
  const build = () => chart(function () { return this.count > 0; });
  assert.notEqual(system.define(build()).id, system.define(build()).id);
  system.close();
}

// Another structure under the same revision is refused (core ADR 0087),
// functions or not.
function changedStructure() {
  const system = createSystem();
  const guard = function () { return true; };
  system.define(chart(guard));
  assert.throws(() => system.define(statechart({ family: 'handles', revision: '1' }).state('only')),
    (error) => error instanceof TinyactorsError && error.code === 'definition-mismatch');
  system.close();
}

// Defining a definition's export answers it, and the definition holds the
// very values the author passed, whatever their type.
function exportAnswers() {
  class Limits { constructor() { this.max = 3; } }
  const limits = new Limits();
  const plain = { a: [1, 2] };
  const system = createSystem();
  const definition = system.define(chart(function () { return this.count < this.limits.max; }, { limits, plain }));
  assert.equal(system.define(definition.export()).id, definition.id);
  const data = Object.fromEntries(definition.export().data.map((item) => [item.name, item.initializer.expr]));
  assert.equal(data.limits.value, limits, 'a class instance is the instance passed');
  assert.equal(data.plain.value, plain, 'a plain object is the object passed, not a copy');
  assert.equal(data.count.value, 0, 'a value exports as the literal() it was, not as JSON text');
  system.close();
}

// A literal() is keyed by the value it holds, as Object.is compares it.
function literalsByValue() {
  const system = createSystem();
  const guard = function () { return true; };
  const define = (value) => system.define(chart(guard, { value })).id;
  // Each distinct value gets its own family, so they do not meet as a mismatch.
  const family = (name, value) => system.define(statechart({ family: `value-${name}`, revision: '1' })
    .data('value', value).state('a')).id;
  assert.equal(family('zero', 0), family('zero', literal(0)), 'literal(0) is literal(0)');
  assert.equal(family('nan', Number.NaN), family('nan', Number.NaN), 'NaN is NaN');
  assert.equal(family('text', 'x'), family('text', 'x'));
  assert.equal(family('big', 10n), family('big', 10n));
  assert.equal(family('none', undefined), family('none', undefined));
  assert.equal(family('null', null), family('null', null));
  assert.notEqual(family('zeros', 0), family('zeros', -0), '-0 is not 0');
  assert.notEqual(family('objects', {}), family('objects', {}), 'two objects are two values');
  const shared = {};
  assert.equal(define(shared), define(shared), 'the same object is the same value');
  system.close();
}

// A literal() encodes as JSON text, which holds a copy of the value: in the
// same system the text is another structure under the same identity, and
// another system defines it under that identity.
function textIsACopy() {
  const system = createSystem();
  const definition = system.define(statechart({ family: 'text', revision: '1' }).data('limit', 3).state('a'));
  const encoded = definition.encode('scxml');
  assert.match(new TextDecoder().decode(encoded.bytes), /<data id="limit" expr="3"\/>/);
  assert.throws(() => system.define(encoded.bytes, { format: 'scxml', identity: encoded.identity }),
    (error) => error instanceof TinyactorsError && error.code === 'definition-mismatch');
  const other = createSystem();
  assert.deepEqual(other.define(encoded.bytes, { format: 'scxml', identity: encoded.identity }).identity,
    definition.identity);
  other.close();
  // A value JSON cannot say exactly has no text.
  const dated = system.define(statechart({ family: 'dated', revision: '1' }).data('at', new Date(0)).state('a'));
  assert.throws(() => dated.encode('scxml'),
    (error) => error instanceof TinyactorsError && error.code === 'invalid' && error.detail.code === 'unrepresentable');
  system.close();
}

// A handle goes when the last definition using it is freed: nothing napi
// keeps holds its function, which can then be collected.
async function released() {
  const system = createSystem();
  let collected = false;
  const registry = new FinalizationRegistry(() => { collected = true; });
  (() => {
    const guard = function () { return this.count > 0; };
    registry.register(guard, 'guard');
    const definition = system.define(chart(guard));
    assert.equal(system.define(chart(guard)).id, definition.id);
    definition.retire();
  })();
  for (let attempt = 0; attempt < 50 && !collected; attempt += 1) {
    gc();
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.ok(collected, 'the function of a freed definition is collected');
  system.close();
}

(async () => {
  sameHandles();
  newClosures();
  changedStructure();
  exportAnswers();
  literalsByValue();
  textIsACopy();
  await released();
  const stats = native.environmentStats();
  assert.equal(stats.leakedBytes, 0, 'every system closed without leaking');
  console.log('handles.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
