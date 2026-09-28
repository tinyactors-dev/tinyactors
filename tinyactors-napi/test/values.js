'use strict';

// Values in text (napi ADR 0031): classes registered by name beside
// functions, the serialize symbol values describe themselves with, how the
// built-in data model reads `new Name(arguments)`, and how data models print
// a chart's values.

const assert = require('node:assert/strict');
const { createSystem, serialize, TinyactorsError } = require('..');

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

(async () => {
  registration();
  await construction();
  constructionRefusals();
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
