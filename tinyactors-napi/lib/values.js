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

module.exports = { serialize, STANDARD_CLASSES };

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

// `text`, which starts with `new` and a space, as a function that builds
// its value; throws an Error whose message is the refusal.
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
  if (parser.at !== text.length || node.new === undefined) {
    parser.fail();
  }
  return function construct() { return build(node); };
}

module.exports.construction = construction;
