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

// ---------------------------------------------------------------------------
// The scanner itself (lib/durable.js), fed pieces cut anywhere: as strings,
// and as UTF-8 bytes split inside a character.

const { durableChunks, durableReader, STANDARD_CLASSES } = require('../lib/durable');

const scannerClasses = { ...STANDARD_CLASSES, Money };

function readPieces(pieces) {
  const reader = durableReader(scannerClasses);
  for (const piece of pieces) {
    reader.push(piece);
  }
  return reader.end();
}

function cutsOf(text, cuts) {
  const pieces = [];
  let at = 0;
  for (const cut of cuts) {
    pieces.push(text.slice(at, cut));
    at = cut;
  }
  pieces.push(text.slice(at));
  return pieces;
}

// Every token kind and escape, cut at every position into two and three pieces.
function everyCut() {
  const text = '{"n":-12.5e+3,"z":0,"t":true,"f":false,"u":null,"s":"a\\"b\\\\c\\/d\\b\\f\\n\\r\\t' +
    '\\u00e9\\uD83D\\uDE00é😀","d":new Date(1700000000000),"m":new Money(5,"EUR"),"a":[[],{}]}';
  const whole = readPieces([text]);
  assert.equal(whole.n, -12500);
  assert.equal(whole.s, 'a"b\\c/d\b\f\n\r\té😀é😀');
  assert.ok(whole.m instanceof Money && whole.d.getTime() === 1700000000000);
  for (let cut = 0; cut <= text.length; cut += 1) {
    assert.deepEqual(readPieces(cutsOf(text, [cut])), whole, `cut at ${cut}`);
  }
  for (let first = 0; first <= text.length; first += 5) {
    for (let second = first; second <= text.length; second += 3) {
      assert.deepEqual(readPieces(cutsOf(text, [first, second])), whole, `cuts at ${first}, ${second}`);
    }
  }
  // As bytes: every cut, including inside é (2 bytes) and 😀 (4 bytes).
  const bytes = Buffer.from(text, 'utf8');
  for (let cut = 0; cut <= bytes.length; cut += 1) {
    assert.deepEqual(readPieces([bytes.subarray(0, cut), bytes.subarray(cut)]), whole, `byte cut at ${cut}`);
  }
  // One byte at a time.
  assert.deepEqual(readPieces([...bytes].map((byte) => Uint8Array.of(byte))), whole);
}

// Random values through the printer, cut at random places, read back.
function fuzz() {
  let seed = 12345;
  const random = () => {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    return seed / 2147483648;
  };
  const alphabet = ['a', 'Z', ' ', '"', '\\', '/', '\n', '\t', '\u0001', '\u001f', 'é', '€', '😀', ' ', '{', ']', ','];
  const randomString = () => Array.from({ length: Math.floor(random() * 12) }, () => alphabet[Math.floor(random() * alphabet.length)]).join('');
  const randomValue = (depth) => {
    const kind = Math.floor(random() * (depth > 4 ? 5 : 8));
    switch (kind) {
      case 0: return null;
      case 1: return random() < 0.5;
      case 2: return Math.round((random() - 0.5) * 1e6) / (random() < 0.5 ? 1 : 1000);
      case 3: case 4: return randomString();
      case 5: return Array.from({ length: Math.floor(random() * 4) }, () => randomValue(depth + 1));
      case 6: {
        const object = {};
        for (let index = Math.floor(random() * 4); index > 0; index -= 1) {
          object[randomString()] = randomValue(depth + 1);
        }
        return object;
      }
      default: return new Date(Math.floor(random() * 2e12));
    }
  };
  for (let round = 0; round < 500; round += 1) {
    const value = randomValue(0);
    const bytes = Buffer.from([...durableChunks(value)].join(''), 'utf8');
    const pieces = [];
    for (let at = 0; at < bytes.length;) {
      const size = 1 + Math.floor(random() * 9);
      pieces.push(bytes.subarray(at, at + size));
      at += size;
    }
    assert.deepEqual(readPieces(pieces), value, `round ${round}`);
  }
}

// Malformed text is refused where it goes wrong, not only at the end.
function refusedWhereItGoesWrong() {
  const cases = [
    ['[1,2x]', /at character 4/],
    ['{"a" 1}', /at character 5/],
    ['"ab\u0001c"', /at character 3/],
    ['"a\\qb"', /at character 3/],
    ['"\\u12G4"', /at character 5/],
    ['[01]', /at character 1/],
    ['[1.]', /at character 1/],
    ['nope', /at character 0/],
    ['1 2', /at character 2/],
    ['"open', /a string is not closed/],
    ['[1,2', /ends before its value does/],
    ['', /ends before its value does/],
  ];
  for (const [text, pattern] of cases) {
    for (const pieces of [[text], [...text]]) {
      assert.throws(() => readPieces(pieces), pattern, JSON.stringify(text));
    }
  }
  assert.throws(() => readPieces(['new Nope(1)']), /no class is registered under Nope/);
  assert.throws(() => readPieces([`[${'1'.repeat(2000)}]`]), /not a value/);
  assert.throws(() => readPieces([Uint8Array.of(0x22, 0xc3, 0x28, 0x22)]), /malformed UTF-8/);
  assert.throws(() => readPieces([Uint8Array.of(0x22, 0xe2, 0x82)]), /malformed UTF-8 at the end/);
}

values();
large();
refusals();
offloadForm();
everyCut();
fuzz();
refusedWhereItGoesWrong();
