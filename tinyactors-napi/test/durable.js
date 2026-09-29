'use strict';

// A JavaScript value as durable bytes (core ADR 0090 section 2, napi ADR
// 0034 section 2): the built-in data model's text, written in pieces and
// read back from pieces of any size; refused values; and the offload form,
// which stays process-local.

const assert = require('node:assert/strict');
const { createSystem, serialize, TinyactorsError } = require('..');
const { nativeOf } = require('../lib/system');

class Money {
  constructor(amount, currency) {
    this.amount = amount;
    this.currency = currency;
  }

  [serialize]() {
    return { name: 'Money', arguments: [this.amount, this.currency] };
  }
}

function roundTrip(system, value, piece) {
  const native = nativeOf(system);
  const { bytes, largest } = native.durableBytes(value, true);
  return { bytes, largest, back: native.durableValue(bytes, piece) };
}

function values() {
  const system = createSystem();
  system.registerClass('Money', Money);
  const value = {
    number: 1.5, text: 'x"\\y\u2028é😀', flags: [true, false, null], nested: { deeper: [[], {}] },
    when: new Date(1700000000000), map: new Map([['a', 1], [2, [3]]]), set: new Set(['s']),
    pattern: /a+b/gi, bytes: new Uint8Array([0, 255]), price: new Money(5, 'EUR'), ['__proto__']: 7,
  };
  for (const piece of [1, 2, 3, 7, 4096, 0]) {
    const { back } = roundTrip(system, value, piece);
    assert.equal(back.number, 1.5);
    assert.equal(back.text, value.text);
    assert.deepEqual(back.flags, [true, false, null]);
    assert.deepEqual(back.nested, { deeper: [[], {}] });
    assert.ok(back.when instanceof Date && back.when.getTime() === 1700000000000);
    assert.ok(back.map instanceof Map && back.map.get('a') === 1 && back.map.get(2)[0] === 3);
    assert.ok(back.set instanceof Set && back.set.has('s'));
    assert.ok(back.pattern instanceof RegExp && back.pattern.source === 'a+b' && back.pattern.flags === 'gi');
    assert.ok(back.bytes instanceof Uint8Array && back.bytes[1] === 255);
    assert.ok(back.price instanceof Money && back.price.amount === 5 && back.price.currency === 'EUR');
    assert.ok(Object.hasOwn(back, '__proto__') && back.__proto__ === 7);
  }
  // The bytes are the built-in data model's text, as UTF-8.
  const { bytes } = roundTrip(system, { a: [1, 'b'] }, 0);
  assert.equal(bytes.toString('utf8'), '{"a":[1,"b"]}');
  system.close();
}

// A large value crosses in pieces: no single write holds the whole text.
function large() {
  const system = createSystem();
  const text = 'é'.repeat(5 * 1024 * 1024); // 10 MB as UTF-8
  const { bytes, largest, back } = roundTrip(system, text, 65536);
  assert.equal(back, text);
  assert.ok(bytes.length > 10 * 1024 * 1024);
  assert.ok(largest <= 64 * 1024, `the largest piece was ${largest} bytes`);
  let deep = [];
  const root = deep;
  for (let depth = 0; depth < 50000; depth += 1) {
    const inner = [];
    deep.push(inner);
    deep = inner;
  }
  const nested = roundTrip(system, root, 4096);
  assert.equal(nested.bytes.length, 2 * 50001);
  let depth = 0;
  for (let at = nested.back; at.length === 1; at = at[0]) {
    depth += 1;
  }
  assert.equal(depth, 50000);
  assert.ok(nested.largest <= 64 * 1024);
  system.close();
}

function refusals() {
  const system = createSystem();
  const native = nativeOf(system);
  const cycle = {};
  cycle.self = cycle;
  class Plain { constructor() { this.x = 1; } }
  for (const [value, pattern] of [
    [() => 1, /function/], [cycle, /cycle/], [new Plain(), /Plain has no text/], [NaN, /NaN/], [10n, /BigInt/],
  ]) {
    assert.throws(() => native.durableBytes(value, true), (error) => {
      assert.ok(error instanceof TinyactorsError, String(error));
      assert.equal(error.code, 'unsupported');
      assert.match(error.message, /no durable form/);
      return true;
    }, String(pattern));
  }
  // An unknown class, and bytes that are no value, are refused on the way back.
  const other = createSystem();
  other.registerClass('Money', Money);
  const { bytes } = nativeOf(other).durableBytes(new Money(1, 'EUR'), true);
  assert.throws(() => native.durableValue(bytes, 3), (error) => error.code === 'unsupported');
  for (const text of ['{"a":', '[1,2', 'nope', '1 2', '{"a" 1}', '']) {
    assert.throws(() => native.durableValue(Buffer.from(text), 1), (error) => error.code === 'unsupported', text);
  }
  other.close();
  system.close();
}

// durable=false is offload's form: a handle or v8's serialization, only for this process.
function offloadForm() {
  const system = createSystem();
  const { bytes } = nativeOf(system).durableBytes({ a: 1 }, false);
  assert.ok(bytes[0] === 0x53 || bytes[0] === 0x48, 'offload bytes start with their tag');
  system.close();
}

values();
large();
refusals();
offloadForm();
