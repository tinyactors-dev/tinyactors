'use strict';

// Chart codecs (napi ADR 0027, core ADR 0053). A codec turns a chart syntax
// into a spec and back. System.registerCodec() keeps them here, one registry
// per system; napi wraps the chosen one in a core codec for the length of one
// define() or Definition.encode() (src/napi_codecs.c), so a codec written in
// JavaScript takes the path the built-in SCXML codec takes.

const { TinyactorsError } = require('./errors');

// Core's chart error codes (TA_CHART_ERROR_*), as index.d.ts spells them.
const CHART_ERROR_CODES = Object.freeze([
  'syntax', 'not-chart', 'unknown-construct', 'misplaced-construct',
  'unknown-attribute', 'missing-attribute', 'conflicting-attribute',
  'bad-value', 'foreign-content', 'src-unresolved', 'limit', 'unrepresentable',
]);

const PLACE = ['line', 'column', 'offset'];
const ABOUT = ['construct', 'attribute'];

/** A codec's refusal: `code` is a ChartErrorCode, with where and what it is about. */
class ChartCodecError extends Error {
  constructor(code, message, options = {}) {
    if (!CHART_ERROR_CODES.includes(code)) {
      throw new TinyactorsError(`ChartCodecError: ${JSON.stringify(code)} is not a chart error code`, 'invalid');
    }
    super(message);
    this.name = 'ChartCodecError';
    this.code = code;
    // Always the codec's own: JavaScriptCore gives every Error a `line` and
    // `column` of where it was thrown, which are not a place in the document.
    for (const key of PLACE) {
      const value = options?.[key];
      if (value !== undefined && !(Number.isSafeInteger(value) && value >= 0)) {
        throw new TinyactorsError(`ChartCodecError: ${key} is a whole number`, 'invalid');
      }
      Object.defineProperty(this, key, { value, writable: true, enumerable: value !== undefined, configurable: true });
    }
    for (const key of ABOUT) {
      const value = options?.[key];
      if (value !== undefined) {
        if (typeof value !== 'string') {
          throw new TinyactorsError(`ChartCodecError: ${key} is a string`, 'invalid');
        }
        this[key] = value;
      }
    }
  }
}

// The built-in SCXML codec: native, in every system.
const SCXML = Object.freeze({
  name: 'application/scxml+xml',
  extensions: Object.freeze(['scxml']),
  description: 'SCXML 1.0 documents (W3C Recommendation)',
  builtin: true,
  decode: true,
  encode: true,
});

function info(codec) {
  const out = { name: codec.name };
  if (codec.extensions !== undefined) out.extensions = [...codec.extensions];
  if (codec.description !== undefined) out.description = codec.description;
  out.decodes = codec.decode !== undefined;
  out.encodes = codec.encode !== undefined;
  return out;
}

class Codecs {
  #entries = [SCXML];

  register(codec) {
    if (codec === null || typeof codec !== 'object') {
      throw new TinyactorsError('registerCodec(): expected { name, extensions?, decode?, encode? }', 'invalid');
    }
    const { name, extensions, description, decode, encode } = codec;
    if (typeof name !== 'string' || name === '') {
      throw new TinyactorsError('registerCodec(): a codec needs a name, such as its media type', 'invalid');
    }
    if (extensions !== undefined && !(Array.isArray(extensions)
        && extensions.every((extension) => typeof extension === 'string' && extension !== '' && !extension.startsWith('.')))) {
      throw new TinyactorsError('registerCodec(): extensions are strings without the dot', 'invalid');
    }
    if (description !== undefined && typeof description !== 'string') {
      throw new TinyactorsError('registerCodec(): description is a string', 'invalid');
    }
    for (const [key, value] of [['decode', decode], ['encode', encode]]) {
      if (value !== undefined && typeof value !== 'function') {
        throw new TinyactorsError(`registerCodec(): ${key} is a function`, 'invalid');
      }
    }
    if (decode === undefined && encode === undefined) {
      throw new TinyactorsError('registerCodec(): a codec decodes, encodes, or both', 'invalid');
    }
    // A format names a codec by its name or an extension, so they share one space.
    for (const key of [name, ...(extensions ?? [])]) {
      const taken = this.#entries.find((entry) => entry.name === key || entry.extensions?.includes(key));
      if (taken !== undefined) {
        throw new TinyactorsError(`registerCodec(): ${JSON.stringify(key)} names the codec ${taken.name} already`, 'invalid');
      }
    }
    this.#entries.push(Object.freeze({
      name,
      extensions: extensions === undefined ? undefined : Object.freeze([...extensions]),
      description,
      decode,
      encode,
    }));
  }

  list() {
    return this.#entries.map(info);
  }

  /** The codec `format` names, by its name or an extension. */
  find(format, where) {
    if (typeof format !== 'string' || format === '') {
      throw new TinyactorsError(`${where}: format is a codec's name or extension, such as "scxml"`, 'invalid');
    }
    const codec = this.#entries.find((entry) => entry.name === format || entry.extensions?.includes(format));
    if (codec === undefined) {
      const known = this.#entries.map((entry) => entry.name).join(', ');
      throw new TinyactorsError(`${where}: no codec is named ${JSON.stringify(format)}; registered: ${known}`, 'invalid');
    }
    return codec;
  }
}

function refusalOf(error) {
  const refused = { code: error.code, message: error.message };
  for (const key of [...PLACE, ...ABOUT]) {
    if (error[key] !== undefined) refused[key] = error[key];
  }
  return { refused };
}

const utf8 = new TextEncoder();

// The functions napi's adapter calls (src/napi_codecs.c): a codec's refusal
// comes back as { refused }, and any other exception propagates as it is.
function decodeCall(codec, context) {
  return (bytes) => {
    try {
      const spec = codec.decode(bytes, context);
      if (spec === null || typeof spec !== 'object') {
        throw new TinyactorsError(`define(): the codec ${codec.name} answered no spec`, 'invalid');
      }
      return { spec };
    } catch (error) {
      if (error instanceof ChartCodecError) return refusalOf(error);
      throw error;
    }
  };
}

function bytesOf(value, codec) {
  if (typeof value === 'string') return utf8.encode(value);
  if (value instanceof Uint8Array) return value;
  throw new TinyactorsError(`encode(): the codec ${codec.name} answered neither bytes nor a string`, 'invalid');
}

// A span a codec reports, with its offset as a number.
function spanOf(span) {
  if (span === null || typeof span !== 'object') return undefined;
  return {
    line: span.line,
    column: span.column,
    end_line: span.end_line,
    end_column: span.end_column,
    offset: span.offset === undefined ? undefined : Number(span.offset),
    length: span.length,
  };
}

function encodeCall(codec, context) {
  return (spec) => {
    try {
      const written = codec.encode(spec, context);
      if (written !== null && typeof written === 'object' && !(written instanceof Uint8Array)) {
        const spans = written.spans;
        if (spans !== undefined && !Array.isArray(spans)) {
          throw new TinyactorsError(`encode(): the codec ${codec.name} answered spans that are not an array`, 'invalid');
        }
        return { bytes: bytesOf(written.bytes, codec), spans: spans?.map(spanOf) };
      }
      return { bytes: bytesOf(written, codec) };
    } catch (error) {
      if (error instanceof ChartCodecError) return refusalOf(error);
      throw error;
    }
  };
}

module.exports = { ChartCodecError, Codecs, SCXML, decodeCall, encodeCall };
