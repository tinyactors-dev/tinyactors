'use strict';

// A JavaScript value as durable bytes (core ADR 0090 section 2, napi ADR
// 0034 section 2): the text napi's built-in data model prints and reads
// (ADR 0031), JSON and `new Name(arguments)`, written and read in pieces.
//
// - durableChunks(value) yields the text in pieces of about PIECE
//   characters. It walks the value with an explicit stack of frames, one
//   per open container, so no piece and no frame holds the whole encoding,
//   and deep values need no JavaScript stack. A long string is escaped in
//   slices. A value the built-in printer refuses throws ChartCodecError.
// - durableReader(classes) scans the text as pieces arrive (strings, or
//   UTF-8 bytes cut anywhere), each piece once, with an explicit stack too,
//   building the value as it goes; end() answers it.
//
// The pieces never hold the whole text; what stays is the value itself, and
// while a long string is read, its decoded runs until its closing quote,
// joined once there.

const { serialize, STANDARD_CLASSES, standardName, described, refuse } = require('./values');

const PIECE = 16384;
const TypedArray = Object.getPrototypeOf(Uint8Array);

// A leaf's text, or a frame for a container; throws a refusal as the
// built-in printer does (lib/values.js).
function open(value) {
  if (value === null || typeof value === 'boolean') {
    return { text: String(value) };
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      refuse(`${Object.is(value, -0) ? '-0' : value} has no text on the built-in data model`);
    }
    return { text: JSON.stringify(value) };
  }
  if (typeof value === 'string') {
    return { string: value };
  }
  if (typeof value !== 'object' && typeof value !== 'function') {
    refuse(`${typeof value === 'bigint' ? 'a BigInt' : String(value)} has no text on the built-in data model`);
  }
  if (typeof value[serialize] === 'function') {
    const description = described(value);
    return construction(description.name, description.arguments, value);
  }
  if (typeof value === 'function') {
    refuse('a function has no text on the built-in data model; register it and call named()');
  }
  const name = standardName(value);
  if (name === 'Date') {
    const time = value.getTime();
    if (Number.isNaN(time)) {
      refuse('an invalid Date has no text');
    }
    return { text: `new Date(${time})` };
  }
  if (name === 'Map') {
    return construction('Map', [{ entries: value }], value);
  }
  if (name === 'Set') {
    return construction('Set', [{ items: value }], value);
  }
  if (name === 'RegExp') {
    return construction('RegExp', [value.source, value.flags], value);
  }
  if (name !== undefined && value instanceof TypedArray) {
    return construction(name, [{ indexed: value }], value);
  }
  const array = Array.isArray(value);
  const prototype = Object.getPrototypeOf(value);
  if (!array && prototype !== Object.prototype && prototype !== null) {
    refuse(`${value.constructor?.name ?? 'this object'} has no text; give it [serialize]() and register its class`);
  }
  if (Object.getOwnPropertySymbols(value).length !== 0) {
    refuse('an object with symbol keys has no text');
  }
  const descriptors = Object.getOwnPropertyDescriptors(value);
  const keys = Object.keys(descriptors).filter((key) => !(array && key === 'length'));
  if (array && keys.length !== value.length) {
    refuse('an array with holes has no text');
  }
  if (keys.some((key) => !descriptors[key].enumerable || !('value' in descriptors[key]))) {
    refuse('an object with accessors or hidden properties has no text');
  }
  if (array) {
    return frame('[', ']', value, indexed(value));
  }
  let index = 0;
  return frame('{', '}', value, () => {
    if (index === keys.length) {
      return undefined;
    }
    const key = keys[index++];
    return { key, value: descriptors[key].value };
  });
}

function frame(opening, close, container, next) {
  return { opening, close, container, next, first: true };
}

function indexed(list) {
  let index = 0;
  return () => (index === list.length ? undefined : { value: list[index++] });
}

// `new Name(args)`: an argument is a value, or a stand-in for a container
// the constructor takes, walked lazily: a Map's entries, a Set's items, a
// typed array's elements.
function construction(name, args, container) {
  let index = 0;
  return frame(`new ${name}(`, ')', container, () => {
    if (index === args.length) {
      return undefined;
    }
    const arg = args[index++];
    if (arg !== null && typeof arg === 'object' && arg.entries instanceof Map) {
      const entries = arg.entries.entries();
      return { frame: frame('[', ']', null, () => {
        const step = entries.next();
        return step.done ? undefined : { frame: frame('[', ']', null, pair(step.value)) };
      }) };
    }
    if (arg !== null && typeof arg === 'object' && arg.items instanceof Set) {
      const items = arg.items.values();
      return { frame: frame('[', ']', null, () => {
        const step = items.next();
        return step.done ? undefined : { value: step.value };
      }) };
    }
    if (arg !== null && typeof arg === 'object' && arg.indexed instanceof TypedArray) {
      return { frame: frame('[', ']', null, indexed(arg.indexed)) };
    }
    return { value: arg };
  });
}

function pair([key, item]) {
  return indexed([key, item]);
}

function* durableChunks(value) {
  let out = '';
  const seen = new Set();
  const stack = [];
  // Emits one value: a leaf's text, a string in slices, or a new frame.
  const start = function* start(item) {
    if (item.frame !== undefined) {
      stack.push(item.frame);
      out += item.frame.opening;
      return;
    }
    const opened = open(item.value);
    if (opened.text !== undefined) {
      out += opened.text;
      return;
    }
    if (opened.string !== undefined) {
      const text = opened.string;
      out += '"';
      for (let at = 0; at < text.length; at += PIECE) {
        out += JSON.stringify(text.slice(at, at + PIECE)).slice(1, -1);
        if (out.length >= PIECE) {
          yield out;
          out = '';
        }
      }
      out += '"';
      return;
    }
    if (seen.has(opened.container)) {
      refuse('a value with a cycle has no text');
    }
    seen.add(opened.container);
    stack.push(opened);
    out += opened.opening;
  };
  yield* start({ value });
  while (stack.length !== 0) {
    if (out.length >= PIECE) {
      yield out;
      out = '';
    }
    const top = stack[stack.length - 1];
    const item = top.next();
    if (item === undefined) {
      out += top.close;
      stack.pop();
      seen.delete(top.container);
      continue;
    }
    if (!top.first) {
      out += ',';
    }
    top.first = false;
    if (item.key !== undefined) {
      out += `${JSON.stringify(item.key)}:`;
    }
    yield* start(item);
  }
  if (out.length !== 0) {
    yield out;
  }
}


// ---------------------------------------------------------------------------
// Reading: a scanner that consumes each piece once. It keeps no text it has
// read: only the token in progress (a number's or a word's few characters,
// or an open string's decoded segments), a stack of the containers it is
// building, and what the grammar expects next. A string's segments are
// gathered in runs of about PIECE characters and joined once, at its closing
// quote. Pieces may be strings or UTF-8 bytes, cut anywhere: bytes go
// through one streaming decoder, so a character split across pieces is
// whole when it is scanned.

const NUMBER = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$/;
const NUMBER_CHARS = /[0-9eE+\-.]*/y;
const WORD_CHARS = /[A-Za-z0-9_$]*/y;
const STRING_STOP = /["\\\u0000-\u001f]/g;
const NOT_TEXT = 'the bytes are not a value in the built-in data model\'s text';
const TOKEN_MAX = 1024;   // the longest number or word the text can hold, and then some
const SHORT_RUN = 32;     // a run of a string this short is copied as code units
const UNITS = 8192;       // code units buffered before they become a string

const LEX_NONE = 0;
const LEX_STRING = 1;
const LEX_NUMBER = 2;
const LEX_WORD = 3;

// The character after a backslash, as the code unit it stands for.
const ESCAPES = { 0x22: 0x22, 0x5c: 0x5c, 0x2f: 0x2f, 0x62: 0x08, 0x66: 0x0c, 0x6e: 0x0a, 0x72: 0x0d, 0x74: 0x09 };

function hexDigit(c) {
  if (c >= 0x30 && c <= 0x39) return c - 0x30;
  if (c >= 0x61 && c <= 0x66) return c - 0x57;
  if (c >= 0x41 && c <= 0x46) return c - 0x37;
  return -1;
}

function isSpace(c) {
  return c === 0x20 || c === 0x0a || c === 0x0d || c === 0x09;
}

function startsWord(c) {
  return (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a) || c === 0x5f || c === 0x24;
}

class DurableReader {
  constructor(classes) {
    this.classes = classes;
    this.stack = [];
    this.expect = 'value';   // value | first (after [ or ( ) | key | keyFirst | colon | after | name | paren | end
    this.root = undefined;
    this.done = false;
    this.lex = LEX_NONE;
    this.parts = null;       // an open string: its runs so far
    this.run = '';           // and the run being gathered
    this.escape = 0;         // 0: none; 1: after a backslash; 2 to 5: that many less 2 hex digits of \u read
    this.hex = 0;
    this.units = null;       // an open string's short runs and escapes, as code units
    this.count = 0;
    this.token = '';         // an open number's or word's characters
    this.tokenAt = 0;        // and where it began
    this.offset = 0;         // characters scanned before the current piece
    this.at = 0;             // where the scan stands in the current piece
    this.decoder = null;
  }

  fail(message) {
    throw new Error(message ?? `${NOT_TEXT} (at character ${this.offset + this.at})`);
  }

  push(piece) {
    if (typeof piece !== 'string') {
      if (this.decoder === null) {
        this.decoder = new TextDecoder('utf-8', { fatal: true });
      }
      try {
        piece = this.decoder.decode(piece, { stream: true });
      } catch {
        this.fail(`${NOT_TEXT} (malformed UTF-8 after character ${this.offset})`);
      }
    }
    this.scan(piece);
  }

  end() {
    if (this.decoder !== null) {
      let rest;
      try {
        rest = this.decoder.decode();
      } catch {
        this.fail(`${NOT_TEXT} (malformed UTF-8 at the end)`);
      }
      this.scan(rest);
    }
    if (this.lex === LEX_NUMBER) {
      this.number();
    } else if (this.lex === LEX_WORD) {
      this.word();
    } else if (this.lex === LEX_STRING) {
      this.fail(`${NOT_TEXT} (a string is not closed)`);
    }
    if (!this.done) {
      this.fail(`${NOT_TEXT} (it ends before its value does)`);
    }
    return this.root;
  }

  // Scans one piece, to its end; a token that may go on stays open.
  scan(text) {
    const length = text.length;
    this.at = 0;
    while (this.at < length) {
      if (this.lex === LEX_STRING) {
        this.scanString(text);
        continue;
      }
      if (this.lex === LEX_NUMBER) {
        if (this.scanToken(text, NUMBER_CHARS)) {
          this.number();
        }
        continue;
      }
      if (this.lex === LEX_WORD) {
        if (this.scanToken(text, WORD_CHARS)) {
          this.word();
        }
        continue;
      }
      const c = text.charCodeAt(this.at);
      if (isSpace(c)) {
        this.at += 1;
        continue;
      }
      if (this.done) {
        this.fail();
      }
      if (c === 0x22) {
        this.at += 1;
        this.lex = LEX_STRING;
        this.parts = [];
        this.run = '';
      } else if (c === 0x2d || (c >= 0x30 && c <= 0x39)) {
        this.lex = LEX_NUMBER;
        this.token = '';
        this.tokenAt = this.offset + this.at;
      } else if (startsWord(c)) {
        this.lex = LEX_WORD;
        this.token = '';
        this.tokenAt = this.offset + this.at;
      } else if (c === 0x5b || c === 0x5d || c === 0x7b || c === 0x7d || c === 0x28 || c === 0x29 ||
                 c === 0x2c || c === 0x3a) {
        this.punct(c);
        this.at += 1;
      } else {
        this.fail();
      }
    }
    this.offset += length;
  }

  // Adds a number's or a word's characters; true when the token ended in this piece.
  scanToken(text, chars) {
    chars.lastIndex = this.at;
    chars.exec(text);
    const end = chars.lastIndex;
    if (end > this.at) {
      this.token += text.slice(this.at, end);
      if (this.token.length > TOKEN_MAX) {
        this.failToken();
      }
    }
    this.at = end;
    return end < text.length;
  }

  scanString(text) {
    const length = text.length;
    while (this.at < length) {
      if (this.escape !== 0) {
        const c = text.charCodeAt(this.at);
        if (this.escape === 1) {
          if (c === 0x75) {
            this.escape = 2;
            this.hex = 0;
          } else {
            const stands = ESCAPES[c];
            if (stands === undefined) {
              this.fail();
            }
            this.unit(stands);
            this.escape = 0;
          }
          this.at += 1;
          continue;
        }
        const digit = hexDigit(c);
        if (digit < 0) {
          this.fail();
        }
        this.hex = this.hex * 16 + digit;
        this.at += 1;
        this.escape += 1;
        if (this.escape === 6) {
          this.unit(this.hex);
          this.escape = 0;
        }
        continue;
      }
      // Up to the next quote, backslash or control character. Short runs,
      // common where escapes come close together, are copied as code units
      // while they are looked at; a long run is found with the regular
      // expression and taken as one slice.
      let stop = this.at;
      const near = Math.min(length, this.at + SHORT_RUN);
      while (stop < near) {
        const c = text.charCodeAt(stop);
        if (c === 0x22 || c === 0x5c || c < 0x20) {
          break;
        }
        stop += 1;
      }
      if (stop === near && near < length) {
        STRING_STOP.lastIndex = near;
        const found = STRING_STOP.exec(text);
        stop = found === null ? length : found.index;
      }
      // A string that closes here with nothing gathered yet, most keys and
      // short values, is one slice.
      if (stop < length && this.count === 0 && this.run.length === 0 && this.parts.length === 0 &&
          text.charCodeAt(stop) === 0x22) {
        const value = text.slice(this.at, stop);
        this.at = stop + 1;
        this.parts = null;
        this.lex = LEX_NONE;
        this.value(value, true);
        return;
      }
      if (stop - this.at <= SHORT_RUN) {
        for (let at = this.at; at < stop; at += 1) {
          this.unit(text.charCodeAt(at));
        }
      } else {
        this.append(text.slice(this.at, stop));
      }
      this.at = stop;
      if (stop === length) {
        return;
      }
      const c = text.charCodeAt(stop);
      this.at += 1;
      if (c === 0x5c) {
        this.escape = 1;
      } else if (c === 0x22) {
        this.flushUnits();
        this.parts.push(this.run);
        const value = this.parts.length === 1 ? this.parts[0] : this.parts.join('');
        this.parts = null;
        this.run = '';
        this.lex = LEX_NONE;
        this.value(value, true);
        return;
      } else {
        this.at -= 1;
        this.fail();   // a raw control character inside a string
      }
    }
  }

  // One UTF-16 code unit of an open string, into the unit buffer.
  unit(code) {
    if (this.units === null) {
      this.units = new Uint16Array(UNITS);
    }
    this.units[this.count] = code;
    this.count += 1;
    if (this.count === UNITS) {
      this.flushUnits();
    }
  }

  flushUnits() {
    if (this.count === 0) {
      return;
    }
    const text = String.fromCharCode.apply(null, this.units.subarray(0, this.count));
    this.count = 0;
    this.appendRun(text);
  }

  // Adds a long slice of an open string: the buffered units go first.
  append(text) {
    this.flushUnits();
    this.appendRun(text);
  }

  // Adds decoded text to an open string, in runs of about PIECE characters.
  appendRun(text) {
    if (this.run.length === 0 && text.length >= PIECE) {
      this.parts.push(text);
      return;
    }
    this.run += text;
    if (this.run.length >= PIECE) {
      this.parts.push(this.run);
      this.run = '';
    }
  }

  // A number or a word that is not text: refused where it began.
  failToken() {
    this.fail(`${NOT_TEXT} (at character ${this.tokenAt})`);
  }

  number() {
    const text = this.token;
    this.lex = LEX_NONE;
    this.token = '';
    // Not text, or not where a value may stand: refused where it began.
    if (!NUMBER.test(text) || (this.expect !== 'first' && this.expect !== 'value')) {
      this.failToken();
    }
    this.value(Number(text), false);
  }

  word() {
    const text = this.token;
    this.lex = LEX_NONE;
    this.token = '';
    switch (this.expect) {
      case 'first':
      case 'value':
        this.expect = 'value';
        if (text === 'true' || text === 'false') {
          this.complete(text === 'true');
        } else if (text === 'null') {
          this.complete(null);
        } else if (text === 'new') {
          this.expect = 'name';
        } else {
          this.failToken();
        }
        return;
      case 'name':
        if (!Object.prototype.hasOwnProperty.call(this.classes, text)) {
          this.fail(`no class is registered under ${text}; register it with registerClass()`);
        }
        this.stack.push({ kind: 'new', constructor: this.classes[text], items: [] });
        this.expect = 'paren';
        return;
      default:
        this.failToken();
    }
  }

  // A number or a string.
  value(value, string) {
    switch (this.expect) {
      case 'first':
      case 'value':
        this.complete(value);
        return;
      case 'keyFirst':
      case 'key':
        if (!string) {
          this.fail();
        }
        this.stack[this.stack.length - 1].key = value;
        this.expect = 'colon';
        return;
      default:
        this.fail();
    }
  }

  punct(c) {
    switch (this.expect) {
      case 'first':
        if (c === 0x5d || c === 0x29) {
          this.close(c);
          return;
        }
        this.expect = 'value';
        this.punct(c);
        return;
      case 'value':
        if (c === 0x5b) {
          this.stack.push({ kind: 'array', items: [] });
          this.expect = 'first';
        } else if (c === 0x7b) {
          this.stack.push({ kind: 'object', object: {}, key: undefined });
          this.expect = 'keyFirst';
        } else {
          this.fail();
        }
        return;
      case 'paren':
        if (c !== 0x28) {
          this.fail();
        }
        this.expect = 'first';
        return;
      case 'keyFirst':
        if (c === 0x7d) {
          this.close(c);
          return;
        }
        this.fail();
        return;
      case 'colon':
        if (c !== 0x3a) {
          this.fail();
        }
        this.expect = 'value';
        return;
      case 'after':
        if (c === 0x2c) {
          this.expect = this.stack[this.stack.length - 1].kind === 'object' ? 'key' : 'value';
          return;
        }
        this.close(c);
        return;
      default:
        this.fail();
    }
  }

  // A finished value: into the container on top, or the root.
  complete(value) {
    const top = this.stack[this.stack.length - 1];
    if (top === undefined) {
      this.root = value;
      this.done = true;
      this.expect = 'end';
      return;
    }
    if (top.kind === 'object') {
      // Defined, not set: `__proto__` is an own property, as JSON.parse makes it.
      Object.defineProperty(top.object, top.key, { value, enumerable: true, writable: true, configurable: true });
    } else {
      top.items.push(value);
    }
    this.expect = 'after';
  }

  close(c) {
    const top = this.stack.pop();
    if (top === undefined || (top.kind === 'array' && c !== 0x5d) || (top.kind === 'object' && c !== 0x7d) ||
        (top.kind === 'new' && c !== 0x29)) {
      this.fail();
    }
    if (top.kind === 'array') {
      this.complete(top.items);
    } else if (top.kind === 'object') {
      this.complete(top.object);
    } else {
      this.complete(Reflect.construct(top.constructor, top.items));
    }
  }
}

function durableReader(classes) {
  return new DurableReader(classes);
}

module.exports = { durableChunks, durableReader, STANDARD_CLASSES };
