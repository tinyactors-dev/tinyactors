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
