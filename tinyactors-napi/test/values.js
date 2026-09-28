'use strict';

// Values in text (napi ADR 0031): classes registered by name beside
// functions, the serialize symbol values describe themselves with, how the
// built-in data model reads `new Name(arguments)`, and how data models print
// a chart's values.

const assert = require('node:assert/strict');
const { createSystem, statechart, named, literal, serialize, ChartCodecError, TinyactorsError } = require('..');
const { ecmascript } = require('../tools/ecmascript-datamodel');

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

class Money {
  constructor(amount, currency) {
    this.amount = amount;
    this.currency = currency;
  }

  [serialize]() {
    return { name: 'Money', arguments: [this.amount, this.currency] };
  }
}

// registerClass(): one name space with registerFunction(), the standard
// classes registered already, and the symbol a registered one.
function registration() {
  assert.equal(serialize, Symbol.for('tinyactors.serialize'));
  const system = createSystem();
  system.registerClass('Money', Money);
  assertCode(() => system.registerClass('Money', Money), 'invalid', /already registered/);
  assertCode(() => system.registerFunction('Money', () => 0), 'invalid', /already registered/);
  system.registerFunction('total', () => 0);
  assertCode(() => system.registerClass('total', Money), 'invalid', /already registered/);
  for (const name of ['Date', 'Map', 'Set', 'RegExp', 'Uint8Array', 'Float64Array', 'BigInt64Array']) {
    assertCode(() => system.registerClass(name, Money), 'invalid', /already registered/);
    assertCode(() => system.registerFunction(name, () => 0), 'invalid', /already registered/);
  }
  assertCode(() => system.registerClass('In', Money), 'invalid', /identifier other than In/);
  assertCode(() => system.registerClass('not a name', Money), 'invalid', /identifier/);
  assertCode(() => system.registerClass('Thing', {}), 'invalid', /constructor/);
  // `__proto__` is a name like any other.
  system.registerClass('__proto__', Money);
  assertCode(() => system.registerFunction('__proto__', () => 0), 'invalid', /already registered/);
  // Each system has its own names.
  const other = createSystem();
  other.registerFunction('Money', () => 0);
  other.close();
  system.close();
  assertCode(() => system.registerClass('Late', Money), 'closed');
}

// A spec whose data are source text, on the built-in data model.
function chart(family, data, transitions = []) {
  return {
    name: family,
    identity: { family: { name: family }, revision: '1' },
    data: Object.entries(data).map(([name, expr]) => ({ name, initializer: { kind: 'expr', expr } })),
    states: [{ kind: 'atomic', id: 'a', transitions }],
  };
}

// The built-in data model reads `new Name(arguments)`: parsed when the
// chart is defined, built with fresh arguments at each evaluation.
async function construction() {
  const system = createSystem();
  system.registerClass('Money', Money);
  const definition = system.define(chart('prices', {
    price: 'new Money(12, "EUR")',
    table: 'new Map([["k", new Money(2, "USD")], ["n", [1, {"__proto__": true, "x": null}]]])',
    when: 'new Date(1700000000000)',
    bytes: 'new Uint8Array([1, 2, 3])',
    tags: ' new   Set ( [ "a" , "b" ] ) ',
    pattern: 'new RegExp("a+", "g")',
    copy: 'null',
    again: 'null',
  }, [{
    kind: 'internal',
    event: 'make',
    actions: [
      { kind: 'assign', assign: { location: 'copy', value: { kind: 'expr', expr: { source: 'new Money(1, "EUR")' } } } },
      { kind: 'assign', assign: { location: 'again', value: { kind: 'expr', expr: 'new Money(1, "EUR")' } } },
    ],
  }, { kind: 'internal', event: 'check', condition: 'new Date(0)', actions: [
    { kind: 'assign', assign: { location: 'copy', value: { kind: 'expr', expr: 'null' } } },
  ] }]));
  const first = system.spawn(definition);
  const second = system.spawn(definition);
  await first.send('noop');
  await second.send('noop');
  const data = first.data();
  assert.ok(data.price instanceof Money);
  assert.deepEqual([data.price.amount, data.price.currency], [12, 'EUR']);
  assert.ok(data.table instanceof Map);
  assert.ok(data.table.get('k') instanceof Money);
  assert.equal(data.table.get('k').currency, 'USD');
  const object = data.table.get('n')[1];
  assert.equal(Object.getPrototypeOf(object), Object.prototype);
  assert.deepEqual(Object.keys(object), ['__proto__', 'x']);
  assert.equal(data.when.getTime(), 1700000000000);
  assert.deepEqual([...data.bytes], [1, 2, 3]);
  assert.ok(data.bytes instanceof Uint8Array);
  assert.deepEqual([...data.tags], ['a', 'b']);
  assert.equal(String(data.pattern), '/a+/g');
  // A new value at each evaluation: per actor, and per action.
  assert.notEqual(data.price, second.data().price);
  assert.notEqual(data.table.get('n'), second.data().table.get('n'));
  await first.send('make');
  assert.ok(first.data().copy instanceof Money);
  assert.notEqual(first.data().copy, first.data().again);
  assert.deepEqual(first.data().copy, first.data().again);
  // A constructed value is truthy as a condition.
  await first.send('check');
  assert.equal(first.data().copy, null);
  system.close();
}

// What refuses a chart when it is defined, naming the expression.
function constructionRefusals() {
  const system = createSystem();
  system.registerClass('Money', Money);
  const refused = (data, pattern) => assertCode(() => system.define(chart('bad', data)), 'invalid', pattern);
  refused({ price: 'new Nope(1)' }, /data\[0\].*no class is registered under this name/);
  refused({ price: 'new Money(12, amount)' }, /takes JSON arguments and registered classes/);
  refused({ price: 'new Money(12, "EUR") + 1' }, /expected a function or source text|takes JSON arguments/);
  refused({ price: 'new Money(12, "EUR"' }, /expected a function or source text|takes JSON arguments/);
  refused({ price: 'new Money(1, new Nope())' }, /no class is registered/);
  refused({ price: 'new toString()' }, /no class is registered/);
  // Deep nesting is refused, not a crash.
  refused({ price: `new Money(${'['.repeat(100000)}${']'.repeat(100000)})` }, /nested too deeply|takes JSON/);
  // Not a location, not a script.
  assertCode(() => system.define({
    name: 'bad',
    states: [{ kind: 'atomic', id: 'a', onentry: [{ actions: [{ kind: 'script', script: { script: 'new Money(1, "x")' } }] }] }],
  }), 'invalid', /new Name\(\) is a value/);
  // A registered function is not a class, and a class is not a function.
  system.registerFunction('zero', () => 0);
  refused({ price: 'new zero()' }, /no class is registered/);
  refused({ price: 'Money()' }, /no function is registered/);
  system.close();
}

const text = (encoded) => new TextDecoder().decode(encoded.bytes);

function assertUnrepresentable(fn, pattern) {
  assert.throws(fn, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, 'invalid', error.message);
    assert.equal(error.detail.code, 'unrepresentable');
    assert.match(error.message, pattern);
    return true;
  });
}

// The built-in data model prints what it reads: a chart of values encodes
// to SCXML and another system that registers the classes reads it back.
async function printing() {
  const system = createSystem();
  system.registerClass('Money', Money);
  const values = {
    price: new Money(12, 'EUR'),
    table: new Map([['k', new Money(2, 'USD')], ['n', [1, { x: null }]]]),
    when: new Date(1700000000000),
    bytes: new Uint8Array([1, 2, 3]),
    tags: new Set(['a', 'b']),
    pattern: /a+/g,
    list: [new Date(1), 'x'],
    plain: { amount: new Money(3, 'CHF'), note: 'n' },
    count: 0,
  };
  let builder = statechart({ family: 'priced', revision: '1' });
  for (const [name, value] of Object.entries(values)) {
    builder = builder.data(name, value);
  }
  const definition = system.define(builder.state('a'));
  const encoded = definition.encode('scxml');
  const written = text(encoded);
  assert.match(written, /expr="new Money\(12, &quot;EUR&quot;\)"/);
  assert.match(written, /expr="new Date\(1700000000000\)"/);
  assert.match(written, /expr="\[new Date\(1\), &quot;x&quot;\]"/);
  assert.doesNotMatch(written, /datamodel=/);
  // Explicitly for the built-in data model, the same text.
  assert.equal(text(definition.encode('scxml', { datamodel: 'javascript' })).includes('new Money'), true);

  const other = createSystem();
  other.registerClass('Money', Money);
  const read = other.define(encoded.bytes, { format: 'scxml', identity: encoded.identity });
  assert.deepEqual(read.identity, definition.identity);
  const actor = other.spawn(read);
  await actor.send('noop');
  const data = actor.data();
  for (const [name, value] of Object.entries(values)) {
    assert.deepEqual(data[name], value, name);
  }
  assert.ok(data.price instanceof Money && data.table.get('k') instanceof Money && data.plain.amount instanceof Money);
  assert.notEqual(data.price, values.price, 'text is a copy');
  other.close();

  // Without the class registered, the text is refused where it is read.
  const bare = createSystem();
  assert.throws(() => bare.define(encoded.bytes, { format: 'scxml', identity: encoded.identity }), /no class is registered/);
  bare.close();
  system.close();
}

// What the built-in printer refuses, with why.
function printingRefusals() {
  const system = createSystem();
  const refused = (value, pattern) => {
    const family = `refused${(refused.count = (refused.count ?? 0) + 1)}`;
    const definition = system.define(statechart({ family, revision: '1' }).data('it', value).state('a'));
    assertUnrepresentable(() => definition.encode('scxml'), pattern);
  };
  refused(NaN, /NaN has no text/);
  refused(-0, /-0 has no text/);
  refused(1n, /a BigInt has no text/);
  refused({ missing: undefined }, /undefined has no text/);
  const cyclic = {};
  cyclic.self = cyclic;
  refused(cyclic, /a cycle/);
  refused(new (class Opaque {})(), /Opaque has no text; give it \[serialize\]\(\)/);
  refused(new Float64Array([NaN]), /NaN has no text/);
  refused({ [serialize]() { return { name: 'not a name', arguments: [] }; } }, /must answer \{ name, arguments \}/);
  // A function is written as a named function.
  const guarded = system.define(statechart({ family: 'guarded', revision: '1' })
    .state('a', (state) => state.on('go', (t) => t.when(function () { return true; }).target('a'))));
  assertUnrepresentable(() => guarded.encode('scxml'), /register it and call named\(\)/);
  system.close();
}

// encode(format, { datamodel }): the chart written for another data model,
// printed by that data model.
function otherDatamodels() {
  const system = createSystem();
  system.registerFunction('ready', () => true);
  const printed = [];
  system.registerDatamodel('custom', {
    compile: () => () => true,
    print(expression) {
      printed.push(expression.kind);
      if (expression.kind === 'literal' && expression.value === 'refuse') {
        throw new ChartCodecError('unrepresentable', 'custom cannot say this');
      }
      if (expression.kind === 'literal' && expression.value === 'crash') {
        throw new RangeError('custom crashed');
      }
      if (expression.kind === 'literal' && expression.value === 'wrong') {
        return 42;
      }
      return expression.kind === 'function' ? 'fn()' : `lit(${JSON.stringify(expression.value)})`;
    },
  });
  system.registerDatamodel('reader', { compile: () => () => true });
  const chart = (value) => system.define(statechart({ family: `c${JSON.stringify(value)}`, revision: '1' })
    .data('it', value)
    .state('a', (state) => state.on('go', (t) => t.when(named('ready')).script(function () {}).target('a'))));
  const written = text(chart(5).encode('scxml', { datamodel: 'custom' }));
  assert.match(written, /datamodel="custom"/);
  assert.match(written, /expr="lit\(5\)"/);
  assert.deepEqual(printed.sort(), ['function', 'literal']);
  assertUnrepresentable(() => chart('refuse').encode('scxml', { datamodel: 'custom' }), /custom cannot say this/);
  assert.throws(() => chart('crash').encode('scxml', { datamodel: 'custom' }), RangeError);
  assertUnrepresentable(() => chart('wrong').encode('scxml', { datamodel: 'custom' }), /must answer a string/);
  // A JavaScript codec gets the spec as the other data model's, printed.
  system.registerCodec({
    name: 'application/vnd.test.peek',
    encode: (spec) => JSON.stringify([spec.datamodel ?? null, spec.data?.[0]?.initializer?.expr ?? null]),
  });
  assert.equal(text(chart(6).encode('application/vnd.test.peek', { datamodel: 'custom' })), '["custom","lit(6)"]');
  // A data model without print, or none of that name, cannot be written to.
  assertCode(() => chart(1).encode('scxml', { datamodel: 'reader' }), 'unsupported', /cannot print/);
  assertCode(() => chart(1).encode('scxml', { datamodel: 'nope' }), 'unsupported', /cannot print/);
  assertCode(() => chart(1).encode('scxml', { datamodel: 'null' }), 'unsupported', /cannot print/);
  assertCode(() => chart(1).encode('scxml', { datamodel: 7 }), 'invalid', /data model name/);
  assertCode(() => system.registerDatamodel('broken', { compile: () => 0, print: 1 }), 'invalid', /print must be a function/);
  system.close();
}

// The example ECMAScript data model gets the registered names as its
// compile scope and prints: a builder chart, inline functions included,
// written for it and read back by another system.
async function ecmascriptPrinting() {
  const setup = (system) => {
    system.registerDatamodel('ecmascript', ecmascript);
    system.registerClass('Money', Money);
    system.registerFunction('bumped', function (context) { return this.count + (context.event.data ?? 1); });
    return system;
  };
  const system = setup(createSystem());
  const values = {
    count: 0,
    price: new Money(12, 'EUR'),
    when: new Date(1700000000000),
    table: new Map([['k', new Money(2, 'USD')]]),
    odd: [NaN, -0, undefined, 10n],
    seen: null,
  };
  let builder = statechart({ family: 'shop', revision: '1' });
  for (const [name, value] of Object.entries(values)) {
    builder = builder.data(name, literal(value));
  }
  const definition = system.define(builder
    .state('open', (state) => state
      .on('buy', (t) => t.when(function () { return this.count < 2; }).assign('count', named('bumped')))
      .on('look', (t) => t.assign('seen', (context) => context.event.name))
      .on('close', (t) => t.when(named('bumped')).target('closed')))
    .state('closed'));
  const encoded = definition.encode('scxml', { datamodel: 'ecmascript' });
  const written = text(encoded);
  assert.match(written, /datamodel="ecmascript"/);
  // The function's source as the engine gives it (Bun reformats it), called.
  assert.match(written, /cond="\(function ?\(\)[^"]*this\.count &lt; 2;[^"]*\)\.call\(this, __context\)"/);
  assert.match(written, /expr="new Money\(12, &quot;EUR&quot;\)"/);
  assert.match(written, /expr="\[NaN, -0, undefined, 10n\]"/);
  assert.match(written, /expr="bumped\(\)"/);

  const other = setup(createSystem());
  const read = other.define(encoded.bytes, { format: 'scxml', identity: encoded.identity });
  assert.deepEqual(read.inspect().datamodel, 'ecmascript');
  const actor = other.spawn(read);
  await actor.send('buy');
  await actor.send('buy', 5);
  await actor.send('buy');
  await actor.send('look');
  const data = actor.data();
  assert.equal(data.count, 6, 'the named function ran twice, then the inline guard held');
  assert.equal(data.seen, 'look', 'an arrow function gets the context');
  assert.ok(data.price instanceof Money);
  assert.deepEqual(data.price, values.price);
  assert.deepEqual(data.when, values.when);
  assert.deepEqual(data.table, values.table);
  assert.deepEqual(data.odd, values.odd);
  await actor.send('close');
  assert.deepEqual(actor.inspect().activeStates, ['closed']);
  other.close();

  // A function whose source is not an expression is refused.
  const method = system.define(statechart({ family: 'method', revision: '1' })
    .state('a', (state) => state.on('go', (t) => t.when({ check() { return true; } }.check).target('a'))));
  assertUnrepresentable(() => method.encode('scxml', { datamodel: 'ecmascript' }), /not an expression/);
  const native = system.define(statechart({ family: 'native', revision: '1' })
    .state('a', (state) => state.on('go', (t) => t.when(Boolean).target('a'))));
  assertUnrepresentable(() => native.encode('scxml', { datamodel: 'ecmascript' }), /no source text/);
  system.close();
}

(async () => {
  registration();
  await construction();
  constructionRefusals();
  await printing();
  printingRefusals();
  otherDatamodels();
  await ecmascriptPrinting();
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
