// The value table (docs/values.md; tinyactors-core/docs/protocol.md,
// "Values") over CBOR tokens: core's ta_cbor_writer and ta_cbor_reader in C
// (wasm/src/cbor.c) write and read one token per call, and this file makes
// the JavaScript values and refuses what the table has no row for, as the
// addon's src/values.c does:
//
//   undefined, null, true, false  simple values 23, 22, 21, 20
//   a number                      an integer when Number.isSafeInteger holds
//                                 and it is not -0, else a float
//   a bigint                      an integer, -2^64 to 2^64-1
//   a string                      text, valid UTF-8: a lone surrogate refused
//   a Uint8Array                  bytes (another typed array refused)
//   an array                      an array, holes as undefined
//   a plain object                a map of its own enumerable string keys,
//                                 in Object.keys order (its prototype
//                                 Object.prototype or null)
//   an Address                    with `addresses`, tag 32 on its text
//                                 (encode); else refused as a class's instance
//
// and back: integers within +-(2^53-1) numbers, others bigints; bytes a
// Uint8Array; tag 32 on text an Address with `addresses` (decode), else its
// text. Refused: TA_ERROR_UNSUPPORTED for a value outside the table (a
// function, a symbol, a Date, a class's instance), TA_ERROR_INVALID for one
// it cannot hold (a bigint past 64 bits, a lone surrogate, nesting deeper
// than 64, which a cycle reaches) and for malformed CBOR. Core checks
// UTF-8, nesting and the shortest forms.
import { Address, addressText, isAddress } from './address.mjs';
import { TinyactorsError } from './errors.mjs';

const UINT = 1, NEGINT = 2, BYTES = 3, TEXT = 4, ARRAY = 5, MAP = 6, TAG = 7, SIMPLE = 8, FLOAT = 9, END = 16;
const WIDE = 0x100;
const LATER = 0xFFFFFFFF; // TA_CBOR_COUNT_LATER: closed by an END
const DEPTH = 64;
const SAFE = Number.MAX_SAFE_INTEGER;

const refuse = (status, message) => new TinyactorsError(message, { status });
const UNSUPPORTED_OBJECT = 'only plain objects, arrays and Uint8Arrays are values (docs/values.md)';

// Writes `value` with a writer in `scratch` (a host's arena or the
// environment's, above the caller's mark); answers [pointer, length] of the
// bytes, a view valid until the call's leave. `addresses`: an Address is
// tag 32 on its text (encode, and a payload toward quickjs).
export function write(module, scratch, value, { addresses = false } = {}) {
  const writer = module.call('ta_web_writer_new', module.env, scratch);
  if (writer === 0) throw refuse('TA_ERROR_NO_MEMORY', 'no memory for a CBOR writer');
  const exports = module.fn;
  const token = (kind, number) => exports.ta_web_write(writer, kind, number);
  const integer = (negative, n) => {
    // n: the CBOR argument (a number below 2^53 or a bigint)
    if (typeof n === 'number' && n <= 0xFFFFFFFF) return token(negative ? NEGINT : UINT, n);
    module.setWord64(0, n);
    return token((negative ? NEGINT : UINT) | WIDE, 0);
  };
  const text = (string) => {
    const [at, length] = module.text(scratch, string);
    module.setWord(0, at);
    module.setWord(1, length);
    token(TEXT, 0);
  };
  const put = (v, depth) => {
    if (depth > DEPTH) throw refuse('TA_ERROR_INVALID', 'a value nested deeper than 64 (or a cycle)');
    switch (typeof v) {
    case 'undefined': return token(SIMPLE, 23);
    case 'boolean': return token(SIMPLE, v ? 21 : 20);
    case 'number':
      if (Number.isSafeInteger(v) && !Object.is(v, -0)) return v >= 0 ? integer(false, v) : integer(true, -1 - v);
      module.setWordFloat(0, v);
      return token(FLOAT, 0);
    case 'bigint': {
      if (v >= 0n) {
        if (v >= 1n << 64n) throw refuse('TA_ERROR_INVALID', 'a bigint past 64 bits');
        return integer(false, v <= BigInt(SAFE) ? Number(v) : v);
      }
      const n = -1n - v;
      if (n >= 1n << 64n) throw refuse('TA_ERROR_INVALID', 'a bigint past 64 bits');
      return integer(true, n <= BigInt(SAFE) ? Number(n) : n);
    }
    case 'string': return text(v);
    case 'object': {
      if (v === null) return token(SIMPLE, 22);
      if (Array.isArray(v)) {
        token(ARRAY, LATER);
        for (let i = 0, n = v.length; i < n; i++) put(v[i], depth + 1);
        return token(END, 0);
      }
      if (ArrayBuffer.isView(v)) {
        if (!(v instanceof Uint8Array)) throw refuse('TA_ERROR_UNSUPPORTED', 'bytes are a Uint8Array, not another typed array');
        const [at, length] = module.copyIn(scratch, v);
        module.setWord(0, at);
        module.setWord(1, length);
        return token(BYTES, 0);
      }
      if (addresses && isAddress(v)) {
        token(TAG, 32);
        return text(addressText(v));
      }
      const prototype = Object.getPrototypeOf(v);
      if (prototype !== null && prototype !== Object.prototype) throw refuse('TA_ERROR_UNSUPPORTED', UNSUPPORTED_OBJECT);
      token(MAP, LATER);
      for (const key of Object.keys(v)) {
        const item = v[key];
        text(key);
        put(item, depth + 1);
      }
      return token(END, 0);
    }
    default:
      throw refuse('TA_ERROR_UNSUPPORTED', 'a function, a symbol or an external is not a value (docs/values.md)');
    }
  };
  put(value, 0);
  const status = module.call('ta_web_writer_bytes', writer);
  if (status !== 0) {
    throw refuse(module.statusName(status), status === -4 ? 'no memory for the encoded value' : 'the CBOR writer refused the value');
  }
  return [module.word(0), module.word(1)];
}

// The value of the CBOR at [pointer, length] (in linear memory, valid while
// it reads), read with a reader in `scratch`, nothing after it.
// `addresses`: tag 32 is an Address (decode), else its text.
export function read(module, scratch, pointer, length, { addresses = false } = {}) {
  module.setWord(0, pointer);
  module.setWord(1, length);
  const reader = module.call('ta_web_reader_new', module.env, scratch);
  if (reader === 0) throw refuse('TA_ERROR_NO_MEMORY', 'no memory for a CBOR reader');
  const exports = module.fn;
  const malformed = (got) => refuse(module.statusName(got < 0 ? got : -3), 'malformed or truncated CBOR');
  const step = () => {
    const got = exports.ta_web_read(reader);
    if (got !== 1) throw malformed(got);
  };
  const next = () => {
    step();
    const kind = module.word(0), count = module.word(1);
    switch (kind) {
    case UINT: {
      const high = module.word(3);
      if (high < 0x200000) return high * 0x100000000 + module.word(2);
      return module.word64(2);
    }
    case NEGINT: {
      const high = module.word(3);
      if (high < 0x1FFFFF || (high === 0x1FFFFF && module.word(2) < 0xFFFFFFFF)) return -1 - (high * 0x100000000 + module.word(2));
      return -1n - module.word64(2);
    }
    case BYTES: return module.copyOut(module.word(4), module.word(5));
    case TEXT: return module.string(module.word(4), module.word(5));
    case ARRAY: {
      const out = new Array(count);
      for (let i = 0; i < count; i++) out[i] = next();
      return out;
    }
    case MAP: {
      const out = {};
      for (let i = 0; i < count; i++) {
        step();
        if (module.word(0) !== TEXT) throw refuse('TA_ERROR_INVALID', 'a map key that is not text');
        const key = module.string(module.word(4), module.word(5));
        // defined, not set: "__proto__" is an own key and no setter runs
        Object.defineProperty(out, key, { value: next(), enumerable: true, writable: true, configurable: true });
      }
      return out;
    }
    case TAG: {
      if (module.word(2) !== 32 || module.word(3) !== 0) throw refuse('TA_ERROR_INVALID', 'a tag other than 32');
      step();
      if (module.word(0) !== TEXT) throw refuse('TA_ERROR_INVALID', 'tag 32 on something but text');
      const text = module.string(module.word(4), module.word(5));
      return addresses ? new Address(text) : text;
    }
    case SIMPLE:
      switch (module.word(2)) {
      case 20: return false;
      case 21: return true;
      case 22: return null;
      case 23: return undefined;
      default: throw refuse('TA_ERROR_INVALID', 'a simple value other than false, true, null, undefined');
      }
    case FLOAT: return module.wordFloat(2);
    default: throw refuse('TA_ERROR_INVALID', 'an item the value table has no row for');
    }
  };
  const value = next();
  const after = exports.ta_web_read(reader);
  if (after !== 0) throw refuse(module.statusName(after < 0 ? after : -3), 'bytes after the item');
  return value;
}
