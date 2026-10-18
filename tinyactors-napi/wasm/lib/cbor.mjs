// Values as CBOR tokens through core's writer and reader in C (wasm/src/
// cbor.c): JavaScript pushes and pulls one token per call and makes the
// values; core checks UTF-8, nesting and the shortest forms. The value
// table's refusals (docs/values.md) are p5-api's; this writes and reads
// what a config and a reply hold.
import { TinyactorsError } from './errors.mjs';

const UINT = 1, NEGINT = 2, BYTES = 3, TEXT = 4, ARRAY = 5, MAP = 6, TAG = 7, SIMPLE = 8, FLOAT = 9;
const WIDE = 0x100;

// Writes `value` with a writer in `scratch`; answers [pointer, length] of
// the bytes, a view valid until the call's leave.
export function write(module, scratch, value) {
  const writer = module.call('ta_web_writer_new', module.env, scratch);
  if (writer === 0) throw new TinyactorsError('no memory for a CBOR writer', { status: 'TA_ERROR_NO_MEMORY' });
  const put = (v, depth) => {
    if (depth > 64) throw new TinyactorsError('a value nested deeper than 64', { status: 'TA_ERROR_INVALID' });
    if (v === null) return module.call('ta_web_write', writer, SIMPLE, 22);
    if (v === undefined) return module.call('ta_web_write', writer, SIMPLE, 23);
    if (v === true || v === false) return module.call('ta_web_write', writer, SIMPLE, v ? 21 : 20);
    if (typeof v === 'number' && Number.isInteger(v) && Number.isSafeInteger(v)) v = BigInt(v);
    if (typeof v === 'bigint') {
      const negative = v < 0n;
      const n = negative ? -1n - v : v;
      if (n >= 1n << 64n) throw new TinyactorsError('an integer past 64 bits', { status: 'TA_ERROR_INVALID' });
      if (n < 0x100000000n) return module.call('ta_web_write', writer, negative ? NEGINT : UINT, Number(n));
      module.setWord64(0, n);
      return module.call('ta_web_write', writer, (negative ? NEGINT : UINT) | WIDE, 0);
    }
    if (typeof v === 'number') {
      module.setWordFloat(0, v);
      return module.call('ta_web_write', writer, FLOAT, 0);
    }
    if (typeof v === 'string') {
      const [at, length] = module.text(scratch, v);
      module.setWord(0, at);
      module.setWord(1, length);
      return module.call('ta_web_write', writer, TEXT, 0);
    }
    if (v instanceof Uint8Array) {
      const [at, length] = module.copyIn(scratch, v);
      module.setWord(0, at);
      module.setWord(1, length);
      return module.call('ta_web_write', writer, BYTES, 0);
    }
    if (Array.isArray(v)) {
      module.call('ta_web_write', writer, ARRAY, v.length);
      for (const item of v) put(item, depth + 1);
      return undefined;
    }
    if (typeof v === 'object') {
      const keys = Object.keys(v);
      module.call('ta_web_write', writer, MAP, keys.length);
      for (const key of keys) {
        put(key, depth + 1);
        put(v[key], depth + 1);
      }
      return undefined;
    }
    throw new TinyactorsError(`a ${typeof v} is not a value`, { status: 'TA_ERROR_INVALID' });
  };
  put(value, 0);
  const status = module.call('ta_web_writer_bytes', writer);
  if (status !== 0) throw module.refusal(status, 'the value could not be written');
  return [module.word(0), module.word(1)];
}

// The value of the CBOR at [pointer, length], read with a reader in
// `scratch`.
export function read(module, scratch, pointer, length) {
  module.setWord(0, pointer);
  module.setWord(1, length);
  const reader = module.call('ta_web_reader_new', module.env, scratch);
  if (reader === 0) throw new TinyactorsError('no memory for a CBOR reader', { status: 'TA_ERROR_NO_MEMORY' });
  const next = () => {
    const got = module.call('ta_web_read', reader);
    if (got !== 1) throw module.refusal(got === 0 ? -3 : got, 'the CBOR could not be read');
    const kind = module.word(0), count = module.word(1);
    switch (kind) {
    case UINT: { const n = module.word64(2); return n <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(n) : n; }
    case NEGINT: { const n = -1n - module.word64(2); return n >= BigInt(Number.MIN_SAFE_INTEGER) ? Number(n) : n; }
    case BYTES: return module.copyOut(module.word(4), module.word(5));
    case TEXT: return module.string(module.word(4), module.word(5));
    case ARRAY: { const out = []; for (let i = 0; i < count; i++) out.push(next()); return out; }
    case MAP: {
      const out = {};
      for (let i = 0; i < count; i++) {
        const key = next();
        Object.defineProperty(out, String(key), { value: next(), enumerable: true, writable: true, configurable: true });
      }
      return out;
    }
    case TAG: return next();
    case SIMPLE: { const s = module.word(2); return s === 20 ? false : s === 21 ? true : s === 22 ? null : undefined; }
    case FLOAT: return module.wordFloat(2);
    default: throw new TinyactorsError(`a CBOR item of kind ${kind}`, { status: 'TA_ERROR_INVALID' });
    }
  };
  return next();
}
