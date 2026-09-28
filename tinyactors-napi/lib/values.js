'use strict';

// Values in text (napi ADR 0031): the symbol a value implements to describe
// itself, and the classes every system registers.

// A registered symbol, so a library's classes can implement it without
// depending on this package: `value[serialize]()` answers
// { name, arguments }, the name its constructor is registered under and the
// arguments that rebuild it.
const serialize = Symbol.for('tinyactors.serialize');

// The standard constructors, registered in every system so data models
// print and read them without the values implementing anything.
const STANDARD_CLASSES = Object.freeze({
  Date, Map, Set, RegExp,
  Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
  Float32Array, Float64Array, BigInt64Array, BigUint64Array,
});


// ---------------------------------------------------------------------------
// `new Name(arguments)` on the built-in data model (napi ADR 0031): a named
// reference to a registered class. The arguments are JSON values, in any
// position of which `new Name(arguments)` may appear again. Nothing is
// evaluated: the text is parsed when the chart is defined, and the result
// is a function that builds the value, with fresh arguments, each time the
// expression is evaluated.
//
// Refusals are Errors whose messages core keeps as the compile error; core
// cuts those at about 90 bytes.

const UNKNOWN_CLASS = 'no class is registered under this name; register it with registerClass()';
const NOT_ARGUMENTS = 'new Name(arguments) takes JSON arguments and registered classes';

const NUMBER = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;
const STRING = /"(?:[^"\\\u0000-\u001f]|\\(?:["\\/bfnrt]|u[0-9A-Fa-f]{4}))*"/y;
const NAME = /[A-Za-z_$][A-Za-z0-9_$]*/y;
const SPACE = /[ \t\n\r]*/y;

// A recursive-descent parser over `text` into a tree of
// { new: constructor, arguments } | { array } | { object: [[key, node]] } |
// { value }; `classes` is the system's table of registered classes.
class Parser {
  constructor(text, classes) {
    this.text = text;
    this.at = 0;
    this.classes = classes;
  }

  fail(message = NOT_ARGUMENTS) {
    throw new Error(message);
  }

  space() {
    SPACE.lastIndex = this.at;
    SPACE.exec(this.text);
    this.at = SPACE.lastIndex;
  }

  match(pattern) {
    this.space();
    pattern.lastIndex = this.at;
    const found = pattern.exec(this.text);
    if (found === null) {
      return undefined;
    }
    this.at = pattern.lastIndex;
    return found[0];
  }

  take(token) {
    this.space();
    if (!this.text.startsWith(token, this.at)) {
      return false;
    }
    this.at += token.length;
    return true;
  }

  expect(token) {
    if (!this.take(token)) {
      this.fail();
    }
  }

  // `new Name(arguments)`, the `new` already taken.
  construction() {
    const name = this.match(NAME);
    if (name === undefined) {
      this.fail();
    }
    if (!Object.prototype.hasOwnProperty.call(this.classes, name)) {
      this.fail(UNKNOWN_CLASS);
    }
    const args = this.list(')', () => this.value(), '(');
    return { new: this.classes[name], arguments: args };
  }

  list(close, item, open) {
    this.expect(open);
    const items = [];
    if (this.take(close)) {
      return items;
    }
    do {
      items.push(item());
    } while (this.take(','));
    this.expect(close);
    return items;
  }

  value() {
    this.space();
    if (this.text.startsWith('new', this.at) && /[ \t\n\r]/.test(this.text[this.at + 3] ?? '')) {
      this.at += 3;
      return this.construction();
    }
    const string = this.match(STRING);
    if (string !== undefined) {
      return { value: JSON.parse(string) };
    }
    const number = this.match(NUMBER);
    if (number !== undefined) {
      return { value: JSON.parse(number) };
    }
    for (const [word, value] of [['true', true], ['false', false], ['null', null]]) {
      if (this.take(word)) {
        return { value };
      }
    }
    if (this.text[this.at] === '[') {
      return { array: this.list(']', () => this.value(), '[') };
    }
    if (this.text[this.at] === '{') {
      return {
        object: this.list('}', () => {
          const key = this.match(STRING);
          if (key === undefined) {
            this.fail();
          }
          this.expect(':');
          return [JSON.parse(key), this.value()];
        }, '{'),
      };
    }
    return this.fail();
  }
}

// A fresh value from a parsed node.
function build(node) {
  if ('value' in node) {
    return node.value;
  }
  if (node.array !== undefined) {
    return node.array.map(build);
  }
  if (node.object !== undefined) {
    const object = {};
    for (const [key, inner] of node.object) {
      // Defined, not set: `__proto__` is an own property, as JSON.parse makes it.
      Object.defineProperty(object, key, { value: build(inner), enumerable: true, writable: true, configurable: true });
    }
    return object;
  }
  return Reflect.construct(node.new, node.arguments.map(build));
}

// `text`, which starts with `new` and a space, or is an array or object
// with `new Name(...)` inside, as a function that builds its value; throws
// an Error whose message is the refusal.
function construction(text, classes) {
  const parser = new Parser(text, classes);
  let node;
  try {
    node = parser.value();
    parser.space();
  } catch (error) {
    if (error instanceof RangeError) {
      throw new Error('new Name(arguments) is nested too deeply');
    }
    throw error;
  }
  if (parser.at !== text.length) {
    parser.fail();
  }
  return function construct() { return build(node); };
}


// ---------------------------------------------------------------------------
// Printing (napi ADR 0031): a chart's host expressions as source text for
// Definition.encode(). A data model prints in its own language; this is the
// built-in data model's printer, which writes only what it reads back:
// JSON values, and `new Name(arguments)` for a value that describes itself
// with [serialize]() and for the standard classes.

let ChartCodecError;
const refuse = (message) => {
  ChartCodecError ??= require('./codecs').ChartCodecError;
  throw new ChartCodecError('unrepresentable', message);
};
const IDENTIFIER = /^[A-Za-z_$][A-Za-z0-9_$]*$/;
const TypedArray = Object.getPrototypeOf(Uint8Array);
const TYPED_ARRAYS = new Set(Object.values(STANDARD_CLASSES).filter((constructor) => constructor.prototype instanceof TypedArray));

// The name a standard class is registered under, when `value` is exactly
// one of its instances (not a subclass's).
function standardName(value) {
  const prototype = Object.getPrototypeOf(value);
  for (const [name, constructor] of Object.entries(STANDARD_CLASSES)) {
    if (prototype === constructor.prototype) {
      return name;
    }
  }
  return undefined;
}

// A value's description, checked: { name, arguments }.
function described(value) {
  const description = value[serialize]();
  if (description === null || typeof description !== 'object' || typeof description.name !== 'string' ||
      !IDENTIFIER.test(description.name) || !Array.isArray(description.arguments)) {
    refuse('[serialize]() must answer { name, arguments } with an identifier and an array');
  }
  return description;
}

class BuiltinPrinter {
  constructor() {
    this.seen = new Set();
  }

  construction(name, args) {
    return `new ${name}(${args.map((item) => this.value(item)).join(', ')})`;
  }

  value(value) {
    if (value === null || typeof value === 'string' || typeof value === 'boolean') {
      return JSON.stringify(value);
    }
    if (typeof value === 'number') {
      if (!Number.isFinite(value) || Object.is(value, -0)) {
        refuse(`${Object.is(value, -0) ? '-0' : value} has no text on the built-in data model`);
      }
      return JSON.stringify(value);
    }
    if (typeof value !== 'object' && typeof value !== 'function') {
      refuse(`${typeof value === 'bigint' ? 'a BigInt' : String(value)} has no text on the built-in data model`);
    }
    if (this.seen.has(value)) {
      refuse('a value with a cycle has no text');
    }
    this.seen.add(value);
    try {
      return this.object(value);
    } finally {
      this.seen.delete(value);
    }
  }

  object(value) {
    if (typeof value[serialize] === 'function') {
      const description = described(value);
      return this.construction(description.name, description.arguments);
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
      return `new Date(${time})`;
    }
    if (name === 'Map') {
      return `new Map([${[...value].map(([key, item]) => `[${this.value(key)}, ${this.value(item)}]`).join(', ')}])`;
    }
    if (name === 'Set') {
      return `new Set([${[...value].map((item) => this.value(item)).join(', ')}])`;
    }
    if (name === 'RegExp') {
      return this.construction('RegExp', [value.source, value.flags]);
    }
    if (name !== undefined && TYPED_ARRAYS.has(STANDARD_CLASSES[name])) {
      return `new ${name}([${Array.from(value, (item) => this.value(item)).join(', ')}])`;
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
      return `[${value.map((item) => this.value(item)).join(', ')}]`;
    }
    return `{${keys.map((key) => `${JSON.stringify(key)}: ${this.value(descriptors[key].value)}`).join(', ')}}`;
  }
}

// The built-in data model's print(expression).
function printBuiltin(expression) {
  if (expression.kind === 'function') {
    refuse('a function has no text on the built-in data model; register it and call named()');
  }
  if (expression.kind === 'location') {
    refuse('a location has no text on the built-in data model; use a data name');
  }
  return new BuiltinPrinter().value(expression.value);
}

// A data model's print as the addon calls it, (kind, value) => text or
// { message }: a ChartCodecError is a refusal; any other exception is kept
// in `thrown`, for the caller to throw once the encode has returned.
function printCall(print) {
  const call = (kind, value) => {
    const expression = kind === 'function' ? { kind, function: value }
      : kind === 'literal' ? { kind, value } : { kind, location: value };
    try {
      const text = print(expression);
      return typeof text === 'string' ? text : { message: 'print() must answer a string' };
    } catch (error) {
      ChartCodecError ??= require('./codecs').ChartCodecError;
      if (!(error instanceof ChartCodecError)) {
        call.thrown = { error };
      }
      return { message: String(error?.message ?? error) };
    }
  };
  return call;
}

// `spec`, a Definition.export(), with every host expression replaced by the
// text `print` answers for it, and `datamodel` as its data model: what a
// JavaScript codec gets when encode() names another data model. A function
// is a function expression, a literal() a literal, and an object with `get`
// and `set` a location; bytes, bigints and source text stay as they are.
function printedSpec(spec, print, datamodel) {
  const { isLiteral } = require('./statechart');
  const walk = (value) => {
    if (typeof value === 'function') {
      return print({ kind: 'function', function: value });
    }
    if (value === null || typeof value !== 'object' || ArrayBuffer.isView(value)) {
      return value;
    }
    if (isLiteral(value)) {
      return print({ kind: 'literal', value: value.value });
    }
    if (typeof value.get === 'function' && typeof value.set === 'function') {
      return print({ kind: 'location', location: value });
    }
    if (Array.isArray(value)) {
      return value.map(walk);
    }
    return Object.fromEntries(Object.entries(value).map(([key, inner]) => [key, walk(inner)]));
  };
  return { ...walk(spec), datamodel };
}

module.exports = { serialize, STANDARD_CLASSES, construction, printBuiltin, printCall, printedSpec };
