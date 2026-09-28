'use strict';

// Named functions (napi ADR 0029): a chart calls a function the system
// registered as the source text `name()`, which named() writes; plain data
// is written as JSON text. Both are content core sees whole, so such a
// chart is answered by identity and round-trips through export and SCXML.

const assert = require('node:assert/strict');
const { createSystem, statechart, named, literal, TinyactorsError } = require('..');

function assertCode(fn, code, pattern) {
  assert.throws(fn, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, code, error.message);
    if (pattern) {
      assert.match(error.message, pattern);
    }
    return true;
  });
}

function withFunctions() {
  const system = createSystem();
  system.registerFunction('ready', function (context) { return this.count >= (context.event.data ?? 1); });
  system.registerFunction('bump', function () { this.count += 1; });
  system.registerFunction('doubled', function () { return this.count * 2; });
  system.registerFunction('eventName', (context) => context.event.name);
  system.registerFunction('items', () => [1, 2, 3]);
  system.registerFunction('addItem', function () { return this.total + this.item; });
  return system;
}

function counter(condition = 'ready') {
  return statechart({ family: 'counter', revision: '1' })
    .initial('counting')
    .data('count', 0)
    .data('total', 0)
    .data('last', null)
    .state('counting', (s) => s
      .on('bump', (t) => t.script(named('bump')))
      .on('sum', (t) => t.forEach(named('items'), 'item', (b) => b.assign('total', named('addItem'))))
      .on('check', (t) => t.when(named(condition)).target('done')))
    .state('done', (s) => s.entry((a) => a.assign('count', named('doubled')).assign('last', named('eventName'))));
}

// A condition, a script, a value and a foreach array call named functions,
// with `this` = the data and the context, as builder functions get them.
async function calls() {
  const system = withFunctions();
  const records = [];
  system.setLogSink((record) => records.push(record));
  const definition = system.define(counter());
  const actor = system.spawn(definition);
  await actor.send('check');
  assert.deepEqual(actor.inspect().activeStates, ['counting'], 'ready() is false at 0');
  await actor.send('bump');
  await actor.send('bump');
  await actor.send('sum');
  assert.equal(actor.inspect().data.total, 6, 'items() is the foreach array');
  await actor.send('check', 2);
  const inspection = actor.inspect();
  assert.deepEqual(inspection.activeStates, ['done'], 'ready() reads this and the context');
  assert.equal(inspection.data.count, 4, 'doubled() is a value');
  assert.equal(inspection.data.last, 'check', 'eventName() reads the context');

  // A log value, a send event and a raise's data.
  const logs = system.define(statechart({ family: 'logs', revision: '1' })
    .initial('a')
    .data('count', 3)
    .state('a', (s) => s
      .entry((a) => a.log('count', named('doubled')).send(named('eventName')))
      .on('*', (t) => t.target('b')))
    .state('b'));
  system.spawn(logs);
  await system.flush();
  assert.equal(records.at(-1).label, 'count');
  assert.equal(records.at(-1).value, 6, 'log() takes named()');
  system.close();
}

// A name nothing is registered under refuses the chart, at the expression.
function refusals() {
  const system = withFunctions();
  assertCode(
    () => system.define(statechart({ family: 'unknown', revision: '1' })
      .initial('a').state('a', (s) => s.on('e', (t) => t.when(named('nope')).target('a')))),
    'invalid',
    /states\[0\]\(a\)\.transitions\[0\]\.condition: no function is registered under this name.*"nope\(\)"/,
  );
  // A condition written as the string `name()` is a call too.
  const text = system.define(statechart({ family: 'text', revision: '1' })
    .initial('a').data('count', 1).state('a', (s) => s.on('e', (t) => t.when('ready()').target('b'))).state('b'));
  assert.ok(text.export().states[0].transitions[0].condition.includes('ready()'));
  // A call is not a location.
  assertCode(
    () => system.define({
      name: 'location',
      states: [{ kind: 'atomic', id: 'a', onentry: [{ actions: [{ kind: 'assign', assign: { location: 'bump()', value: { kind: 'expr', expr: '1' } } }] }] }],
    }),
    'invalid',
    /a location cannot be a call/,
  );
  // Registering: once per name, an identifier, not In, and a function.
  assertCode(() => system.registerFunction('ready', () => true), 'invalid', /already registered/);
  assertCode(() => system.registerFunction('In', () => true), 'invalid', /identifier other than In/);
  assertCode(() => system.registerFunction('not a name', () => true), 'invalid', /identifier/);
  assertCode(() => system.registerFunction('fine', 42), 'invalid', /expected a function/);
  assert.throws(() => named('In'), TinyactorsError);
  assert.throws(() => named('x()'), TinyactorsError);
  // `__proto__` is a name like any other.
  system.registerFunction('__proto__', () => true);
  system.define(statechart({ family: 'proto', revision: '1' })
    .initial('a').state('a', (s) => s.on('e', (t) => t.when(named('__proto__')).target('a'))));
  system.close();
  assertCode(() => system.registerFunction('late', () => true), 'closed');
}

// Content core sees whole: answered by identity, refused under a changed
// chart, and round-tripped through export and SCXML.
function identity() {
  const system = withFunctions();
  const first = system.define(counter());
  const again = system.define(counter());
  assert.equal(again.id, first.id, 'a chart whose functions are all named is answered by identity');
  assert.equal(system.define(first.export()).id, first.id, 'defining the export answers it too');
  assertCode(() => system.define(counter('bump')), 'definition-mismatch');

  const encoded = first.encode('scxml');
  const text = new TextDecoder().decode(encoded.bytes);
  assert.match(text, /cond="ready\(\)"/);
  assert.match(text, /<script>bump\(\)<\/script>/);
  assert.match(text, /<data id="count" expr="0"\/>/);
  assert.equal(system.define(encoded.bytes, { format: 'scxml', identity: encoded.identity }).id, first.id,
    'encode to SCXML, then define, answers the same definition');

  // In another system that registers the same names, the text defines the
  // same revision.
  const other = withFunctions();
  const copy = other.define(encoded.bytes, { format: 'scxml', identity: encoded.identity });
  assert.deepEqual(copy.identity, first.identity);
  other.close();

  // One anonymous function is enough to register beside (core ADR 0077).
  const anonymous = () => statechart({ family: 'anonymous', revision: '1' })
    .initial('a').data('count', 0).state('a', (s) => s.on('e', (t) => t.when(function () { return true; }).target('a')));
  assert.notEqual(system.define(anonymous()).id, system.define(anonymous()).id);
  system.close();
}

// Plain data is JSON text, parsed once when the chart is defined; what JSON
// cannot say exactly stays a literal().
async function data() {
  const system = createSystem();
  const date = new Date(0);
  const definition = system.define(statechart({ family: 'data', revision: '1' })
    .initial('a')
    .data('object', { nested: [1, 'two', null, true] })
    .data('text', 'hello')
    .data('negative', -1.5)
    .data('date', date)
    .data('missing', undefined)
    .data('notANumber', Number.NaN)
    .data('fn', function kept() { return 1; })
    .data('forced', literal(7))
    .state('a'));
  const spec = definition.export();
  const byName = Object.fromEntries(spec.data.map((item) => [item.name, item.initializer?.expr]));
  assert.equal(byName.object, '{"nested":[1,"two",null,true]}');
  assert.equal(byName.text, '"hello"');
  assert.equal(byName.negative, '-1.5');
  assert.equal(byName.date.value, date, 'a Date stays a literal()');
  assert.ok(Number.isNaN(byName.notANumber.value), 'NaN stays a literal()');
  assert.equal(typeof byName.fn.value, 'function', 'a function in data() stays data');
  assert.equal(byName.forced.value, 7, 'literal() stays a literal()');
  const actor = system.spawn(definition);
  await system.flush();
  const values = actor.inspect().data;
  assert.deepEqual(values.object, { nested: [1, 'two', null, true] });
  assert.equal(values.text, 'hello');
  assert.equal(values.date, date);
  assert.equal(values.fn(), 1);
  // JSON text in a spec is a value: not a script, and it must parse.
  assertCode(() => system.define({ name: 'script', states: [{ kind: 'atomic', id: 'a' }], script: '1' }), 'invalid', /cannot be a location or a script/);
  assertCode(() => system.define({
    name: 'sum',
    states: [{ kind: 'atomic', id: 'a' }],
    data: [{ name: 'x', initializer: { kind: 'expr', expr: '1 + 2' } }],
  }), 'invalid', /expected a function, or source text/);
  system.close();
}

// A registered data model compiles its own source text and does not see
// the names.
function registeredModels() {
  const system = withFunctions();
  const seen = [];
  system.registerDatamodel('custom', {
    compile(source, role) {
      seen.push([source, role]);
      return () => true;
    },
  });
  system.define({
    name: 'custom',
    datamodel: 'custom',
    states: [{ kind: 'atomic', id: 'a', transitions: [{ kind: 'external', event: 'e', condition: 'nope()', targets: ['a'] }] }],
  });
  assert.deepEqual(seen, [['nope()', 'condition']]);
  system.close();
}

(async () => {
  await calls();
  refusals();
  identity();
  await data();
  registeredModels();
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
