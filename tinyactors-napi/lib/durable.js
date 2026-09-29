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
// - durableReader(classes) parses the text as pieces arrive, with an
//   explicit stack too, building the value as it goes; end() answers it.
//
// The pieces never hold the whole text; what stays is the value itself, and
// while a long string is read, its escaped text until its closing quote.

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
// Reading. The parser keeps the text it has not consumed yet, a stack of the
// containers it is building, and what it expects next.

const NUMBER = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;
const NUMBER_TAIL = /^[0-9.eE+-]*$/;
const NAME = /[A-Za-z_$][A-Za-z0-9_$]*/y;
const SPACE = /[ \t\n\r]*/y;
const NOT_TEXT = 'the bytes are not a value in the built-in data model\'s text';

class DurableReader {
  constructor(classes) {
    this.classes = classes;
    this.text = '';
    this.at = 0;
    this.stack = [];
    this.expect = 'value';   // value | first (after [ or ( ) | key | keyFirst | colon | after | name | paren | end
    this.root = undefined;
    this.done = false;
    this.string = -1;        // while a string is open: where its scan stopped
    this.escaped = false;
  }

  fail(message = NOT_TEXT) {
    throw new Error(message);
  }

  push(piece) {
    this.text = this.text.slice(this.at) + piece;
    if (this.string >= 0) {
      this.string -= this.at;
    }
    this.at = 0;
    this.run(false);
  }

  end() {
    this.run(true);
    SPACE.lastIndex = this.at;
    SPACE.exec(this.text);
    if (!this.done || SPACE.lastIndex !== this.text.length) {
      this.fail();
    }
    return this.root;
  }

  // Consumes every whole token; stops at one that may continue in a later piece.
  run(final) {
    for (;;) {
      SPACE.lastIndex = this.at;
      SPACE.exec(this.text);
      this.at = SPACE.lastIndex;
      if (this.at === this.text.length) {
        return;
      }
      if (this.done) {
        this.fail();
      }
      const token = this.token(final);
      if (token === undefined) {
        return;
      }
      this.step(token);
    }
  }

  // The next token, or undefined when it may run on into the next piece.
  token(final) {
    const c = this.text[this.at];
    if ('[]{}(),:'.includes(c)) {
      this.at += 1;
      return { punct: c };
    }
    if (c === '"') {
      return this.stringToken(final);
    }
    if (c === '-' || (c >= '0' && c <= '9')) {
      NUMBER.lastIndex = this.at;
      const found = NUMBER.exec(this.text);
      if (found === null) {
        this.fail();
      }
      // "1." or "1e" may still become a longer number in the next piece.
      if (!final && NUMBER_TAIL.test(this.text.slice(NUMBER.lastIndex))) {
        return undefined;
      }
      this.at = NUMBER.lastIndex;
      return { value: JSON.parse(found[0]) };
    }
    NAME.lastIndex = this.at;
    const found = NAME.exec(this.text);
    if (found === null) {
      this.fail();
    }
    if (NAME.lastIndex === this.text.length && !final) {
      return undefined;
    }
    this.at = NAME.lastIndex;
    return { word: found[0] };
  }

  stringToken(final) {
    let scan = this.string >= 0 ? this.string : this.at + 1;
    for (; scan < this.text.length; scan += 1) {
      const c = this.text.charCodeAt(scan);
      if (this.escaped) {
        this.escaped = false;
      } else if (c === 0x5c) {
        this.escaped = true;
      } else if (c === 0x22) {
        const raw = this.text.slice(this.at, scan + 1);
        this.at = scan + 1;
        this.string = -1;
        return { value: JSON.parse(raw) };
      }
    }
    if (final) {
      this.fail();
    }
    this.string = scan;
    return undefined;
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

  close(punct) {
    const top = this.stack.pop();
    if (top === undefined || (top.kind === 'array' && punct !== ']') || (top.kind === 'object' && punct !== '}') ||
        (top.kind === 'new' && punct !== ')')) {
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

  step(token) {
    const { punct } = token;
    switch (this.expect) {
      case 'first':
        if (punct === ']' || punct === ')') {
          this.close(punct);
          return;
        }
        this.expect = 'value';
        this.step(token);
        return;
      case 'value':
        if ('value' in token) {
          this.complete(token.value);
        } else if (punct === '[') {
          this.stack.push({ kind: 'array', items: [] });
          this.expect = 'first';
        } else if (punct === '{') {
          this.stack.push({ kind: 'object', object: {}, key: undefined });
          this.expect = 'keyFirst';
        } else if (token.word === 'true' || token.word === 'false' || token.word === 'null') {
          this.complete(token.word === 'null' ? null : token.word === 'true');
        } else if (token.word === 'new') {
          this.expect = 'name';
        } else {
          this.fail();
        }
        return;
      case 'name': {
        if (token.word === undefined || !Object.prototype.hasOwnProperty.call(this.classes, token.word)) {
          this.fail(`no class is registered under ${token.word ?? 'this name'}; register it with registerClass()`);
        }
        this.stack.push({ kind: 'new', constructor: this.classes[token.word], items: [] });
        this.expect = 'paren';
        return;
      }
      case 'paren':
        if (punct !== '(') {
          this.fail();
        }
        this.expect = 'first';
        return;
      case 'keyFirst':
        if (punct === '}') {
          this.close(punct);
          return;
        }
        this.expect = 'key';
        this.step(token);
        return;
      case 'key':
        if (typeof token.value !== 'string') {
          this.fail();
        }
        this.stack[this.stack.length - 1].key = token.value;
        this.expect = 'colon';
        return;
      case 'colon':
        if (punct !== ':') {
          this.fail();
        }
        this.expect = 'value';
        return;
      case 'after': {
        const top = this.stack[this.stack.length - 1];
        if (punct === ',') {
          this.expect = top.kind === 'object' ? 'key' : 'value';
          return;
        }
        this.close(punct);
        return;
      }
      default:
        this.fail();
    }
  }
}

function durableReader(classes) {
  return new DurableReader(classes);
}

module.exports = { durableChunks, durableReader, STANDARD_CLASSES };
