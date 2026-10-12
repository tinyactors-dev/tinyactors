// The value table (docs/values.md) through the package's encode and
// decode: values.txt's ECMAScript lines, and each row of the table with
// the JavaScript value it must give, so a row mapped wrong the same way
// in both directions fails here (ROADMAP.md, M1: the vectors alone cannot
// catch it).
import assert from 'node:assert/strict';
import { test, run } from './harness.mjs';
import { encode, decode, TinyactorsError } from '@tinyactors/node';
import { values, hex, toHex } from './vectors.mjs';

const INVALID = -3;

function refused(body, status = INVALID) {
  assert.throws(body, (error) => error instanceof TinyactorsError && error.status === status);
}

// ---- values.txt, ECMAScript's lines ---------------------------------------------

for (const vector of values()) {
  if (vector.name.startsWith('not ECMAScript:')) continue;
  const name = `values.txt ${vector.kind} ${vector.name}`;
  switch (vector.kind) {
    case 'roundtrip':
      test(name, () => assert.equal(toHex(encode(decode(hex(vector.cbor)))), vector.back ?? vector.cbor));
      break;
    case 'encode':
      test(name, () => assert.equal(toHex(encode(new Function(`return (${vector.ecmascript})`)())), vector.cbor));
      break;
    case 'invalid':
      test(name, () => refused(() => encode(new Function(`return (${vector.ecmascript})`)())));
      break;
    case 'malformed':
      test(name, () => assert.throws(() => decode(hex(vector.cbor)), TinyactorsError));
      break;
    default:
      test(name, () => assert.fail(`unknown kind ${vector.kind}`));
  }
}

// ---- docs/values.md, row by row: what decode gives ---------------------------------

test('integers within ±(2^53-1) are numbers', () => {
  assert.equal(decode(hex('1b001fffffffffffff')), 9007199254740991);
  assert.equal(decode(hex('3b001ffffffffffffe')), -9007199254740991);
  assert.equal(decode(hex('00')), 0);
});

test('other integers are BigInts, -2^64 … 2^64-1', () => {
  assert.equal(decode(hex('1b0020000000000000')), 9007199254740992n);
  assert.equal(decode(hex('3b001fffffffffffff')), -9007199254740992n);
  assert.equal(decode(hex('1bffffffffffffffff')), 18446744073709551615n);
  assert.equal(decode(hex('3bffffffffffffffff')), -18446744073709551616n);
});

test('a BigInt is written as an integer; past 64 bits it is refused', () => {
  assert.equal(toHex(encode(5n)), '05');
  assert.equal(toHex(encode(-18446744073709551616n)), '3bffffffffffffffff');
  assert.equal(toHex(encode(18446744073709551615n)), '1bffffffffffffffff');
  refused(() => encode(18446744073709551616n));
  refused(() => encode(-18446744073709551617n));
});

test('a number is an integer when it is a safe integer, else a float', () => {
  assert.equal(toHex(encode(1)), '01');
  assert.equal(toHex(encode(-1)), '20');
  assert.equal(toHex(encode(-0)), 'f98000');
  assert.equal(toHex(encode(1.5)), 'f93e00');
  assert.equal(toHex(encode(9007199254740992)), 'fa5a000000');
  assert.equal(toHex(encode(NaN)), 'f97e00');
  assert.equal(toHex(encode(Infinity)), 'f97c00');
});

test('floats are numbers; a float holding an integer is a number', () => {
  assert.equal(decode(hex('fb3fb999999999999a')), 0.1);
  assert.equal(decode(hex('f93c00')), 1);
  assert.ok(Object.is(decode(hex('f98000')), -0));
});

test('text is a string; a lone surrogate is refused when written', () => {
  assert.equal(decode(hex('63c3bc78')), 'üx');
  assert.equal(toHex(encode('ü✓😀')), '69c3bce29c93f09f9880');
  refused(() => encode('\ud800'));
  refused(() => encode('a\udc00b'));
  refused(() => encode({ ['\ud800']: 1 }));
});

test('bytes are read as a Uint8Array', () => {
  const bytes = decode(hex('43010203'));
  assert.ok(bytes instanceof Uint8Array);
  assert.deepEqual([...bytes], [1, 2, 3]);
});

test('an ArrayBuffer, a typed array and a DataView are written as their own bytes', () => {
  const buffer = new Uint8Array([9, 1, 2, 3, 9]).buffer;
  assert.equal(toHex(encode(buffer)), '450901020309');
  assert.equal(toHex(encode(new Uint8Array(buffer, 1, 3))), '43010203');
  assert.equal(toHex(encode(new DataView(buffer, 1, 2))), '420102');
  assert.equal(toHex(encode(new Uint16Array([1]))), '420100');
});

test('arrays are arrays; undefined in an array is written null', () => {
  assert.deepEqual(decode(hex('83010203')), [1, 2, 3]);
  assert.equal(toHex(encode([1, undefined])), '8201f6');
});

test('maps with text keys are objects, keys in the order written', () => {
  const value = decode(hex('a26162016161 02'.replace(' ', '')));
  assert.deepEqual(Object.keys(value), ['b', 'a']);
  assert.deepEqual(value, { b: 1, a: 2 });
});

test('a key __proto__ is an ordinary key', () => {
  const value = decode(hex('a1695f5f70726f746f5f5f01'));
  assert.equal(Object.getPrototypeOf(value), Object.prototype);
  assert.deepEqual(Object.keys(value), ['__proto__']);
  assert.equal(toHex(encode(value)), 'a1695f5f70726f746f5f5f01');
});

test('objects keep what JSON.stringify keeps', () => {
  assert.equal(toHex(encode({ a: undefined, f() {}, [Symbol('s')]: 1, b: 1 })), 'a1616201');
  assert.equal(toHex(encode({ toJSON: () => 'x' })), '6178');
  assert.equal(toHex(encode(new Date(0))), '7818313937302d30312d30315430303a30303a30302e3030305a');
});

test('false, true, null are themselves', () => {
  assert.equal(decode(hex('f4')), false);
  assert.equal(decode(hex('f5')), true);
  assert.equal(decode(hex('f6')), null);
  assert.equal(toHex(encode([false, true, null])), '83f4f5f6');
});

test('undefined is read as undefined, and written null at the top', () => {
  assert.equal(decode(hex('f7')), undefined);
  assert.deepEqual(decode(hex('81f7')), [undefined]);
  assert.equal(toHex(encode(undefined)), 'f6');
});

test('tag 32, an actor reference, is read as its address text', () => {
  const address = decode(hex('d8206a235f7363786d6c5f3432'));
  assert.equal(address, '#_scxml_42');
  assert.equal(toHex(encode(address)), '6a235f7363786d6c5f3432');
});

test('a value shared twice is copied each time it occurs', () => {
  const shared = [1];
  assert.equal(toHex(encode([shared, shared])), '8281018101');
});

test('a cycle and nesting deeper than 64 are refused', () => {
  const cycle = [];
  cycle.push(cycle);
  refused(() => encode(cycle));
  let deep = 1;
  for (let i = 0; i < 65; i++) deep = [deep];
  refused(() => encode(deep));
  let deepest = 1;
  for (let i = 0; i < 64; i++) deepest = [deepest];
  encode(deepest);
});

test('anything else is refused when read', () => {
  for (const bytes of ['a1010203', 'c16161', 'd82001', 'f0', '6261', '62c3', '0000', '9fff'])
    assert.throws(() => decode(hex(bytes)), TinyactorsError, bytes);
});

test('decode reads an ArrayBuffer, a typed array or a DataView; anything else is refused', () => {
  assert.equal(decode(new Uint8Array([1]).buffer), 1);
  assert.equal(decode(new DataView(new Uint8Array([2]).buffer)), 2);
  refused(() => decode('01'));
  refused(() => decode());
});

test('an exception a getter throws while writing propagates', () => {
  const value = { get a() { throw new RangeError('mine'); } };
  assert.throws(() => encode(value), RangeError);
});

await run();
