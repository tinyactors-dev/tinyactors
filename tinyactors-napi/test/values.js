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

(async () => {
  registration();
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
