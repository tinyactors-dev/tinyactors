'use strict';

// An example ECMAScript data model for SCXML documents (napi ADR 0019). It
// is not part of the package: it runs expression text with `new Function`,
// with the process's rights, which is a choice a user makes for documents
// they trust. For untrusted documents, compile with a sandbox instead (an
// interpreter compiled to WebAssembly, for example); `node:vm` is not a
// security boundary.
//
//   system.registerDatamodel('ecmascript', ecmascript);
//   system.define(xmlText, { format: 'scxml' });
//
// SCXML's ECMAScript data model (SCXML B.2) keeps data as variables, so an
// expression reads `count` for the data item `count`. Each compiled function
// runs its source inside `with (scope) with (this)`: `this` is the actor's
// data (napi's built-in model binds it), and `scope` holds SCXML's system
// variables, read from napi's evaluation context when first used:
//
//   _event        { name, type, sendid, origin, origintype, invokeid, data }
//   _sessionid    _name    _ioprocessors    In(stateID)
//
// and the system's registered names (napi ADR 0031), which compile gets as
// its scope: each registered class under its name, so `new Money(1, "EUR")`
// builds one, and each registered function under its name, called with
// `this` = the data and the context as its argument, as the built-in data
// model calls `name()`. `__context` is napi's evaluation context itself,
// which the text print() writes for a function passes on.
//
// print(expression) is compile's inverse: a chart's host expression as an
// ECMAScript expression, so encode('scxml', { datamodel: 'ecmascript' })
// writes a builder chart as a document this data model reads back. A
// literal is a JavaScript literal: primitives (NaN, -0, undefined, 1n
// included), arrays and plain objects, and `new Name(...)` for a value with
// [serialize]() and for Date, Map, Set, RegExp and typed arrays. A function
// is its source text, called: `(function () { ... }).call(this, __context)`.
// That is faithful only when the function uses nothing but `this` and its
// argument: its source does not carry what it closes over. Name such a
// function with registerFunction() instead.

const ROLES = new Set(['value', 'condition', 'location', 'script']);
const IDENTIFIER = /^[A-Za-z_$][A-Za-z0-9_$]*$/;

// SCXML's _event from napi's event: SCXML spells the fields in lowercase.
function scxmlEvent(event) {
  if (event === undefined) {
    return undefined;
  }
  return {
    name: event.name,
    type: event.type,
    sendid: event.sendID,
    origin: event.origin,
    origintype: event.originType,
    invokeid: event.invocationID,
    data: event.data,
  };
}

// SCXML compares system variables by identity (tests 326, 329: a copy taken
// earlier == the variable now), and napi builds a fresh object each call.
// So each actor's data keeps the last _event and _ioprocessors it saw, and
// hands out the same object while the event, or the processors, are the
// same.
const kept = new WeakMap(); // the actor's data -> { event, io }

function keptFor(data) {
  let entry = kept.get(data);
  if (entry === undefined) {
    entry = {};
    kept.set(data, entry);
  }
  return entry;
}

function sameEvent(data, event) {
  const key = [event.name, event.type, event.sendID, event.origin, event.originType, event.invocationID].join('\u0000');
  const entry = keptFor(data);
  if (entry.event === undefined || entry.event.key !== key || entry.event.data !== event.data) {
    entry.event = { key, data: event.data, value: scxmlEvent(event) };
  }
  return entry.event.value;
}

function sameProcessors(data, processors) {
  const key = JSON.stringify(processors);
  const entry = keptFor(data);
  if (entry.io === undefined || entry.io.key !== key) {
    entry.io = { key, value: processors };
  }
  return entry.io.value;
}

// The system variables of one call, each read only when an expression uses
// it. Assigning one is an error.execution (SCXML 5.10). Compiled code is
// not strict, where assigning to a getter is silently ignored, so each has
// a setter that throws.
function scope(context, data, names) {
  const readOnly = (name) => () => { throw new TypeError(`${name} is a system variable and cannot be assigned`); };
  const variables = {
    get _event() {
      const event = context.event;
      return event === undefined ? undefined : sameEvent(data, event);
    },
    set _event(_) { readOnly('_event')(); },
    get _sessionid() { return String(context.sessionID); },
    set _sessionid(_) { readOnly('_sessionid')(); },
    get _name() { return context.name; },
    set _name(_) { readOnly('_name')(); },
    get _ioprocessors() { return sameProcessors(data, context.ioProcessors); },
    set _ioprocessors(_) { readOnly('_ioprocessors')(); },
    In(id) { return context.inState(id); },
    get __context() { return context; },
  };
  // Registered names first, so a system variable is never shadowed.
  const registered = Object.create(null);
  for (const [name, run] of Object.entries(names?.functions ?? {})) {
    Object.defineProperty(registered, name, { get: () => () => run.call(data, context), enumerable: true });
  }
  for (const [name, constructor] of Object.entries(names?.classes ?? {})) {
    Object.defineProperty(registered, name, { value: constructor, enumerable: true });
  }
  return Object.defineProperties(registered, Object.getOwnPropertyDescriptors(variables));
}

// `new Function` over `body`, run inside the scope and the data.
function scoped(body, ...names) {
  return new Function('__scope', ...names, `with (__scope) { with (this) { ${body} } }`);
}

// SCXML makes an expression that cannot be evaluated an error.execution
// when it runs (SCXML 5.9.2), not a refused document, so a syntax error is
// deferred to the call: the compiled function throws it.
function deferred(error, role) {
  const fail = () => { throw error; };
  return role === 'location' ? { get: fail, set: fail } : fail;
}

function compile(source, role, names) {
  if (!ROLES.has(role)) {
    throw new TypeError(`unknown role ${role}`);
  }
  try {
    return compileNow(source, role, names);
  } catch (error) {
    if (error instanceof SyntaxError) {
      return deferred(error, role);
    }
    throw error;
  }
}

// The names a script declares at its top level with var, let, const or
// function. In SCXML they are the data model's variables (SCXML B.2), so
// they outlive the script; a `new Function` body keeps them local. This is
// a scan, not a parse: good enough for the example, not for every program.
const DECLARED = /(?:^|[;{}\n])\s*(?:(?:var|let|const)\s+([A-Za-z_$][\w$]*)|function\s*\*?\s*([A-Za-z_$][\w$]*)\s*\()/g;

function declaredNames(source) {
  const names = new Set();
  for (const match of source.matchAll(DECLARED)) {
    names.add(match[1] ?? match[2]);
  }
  return [...names];
}

function compileNow(source, role, names) {
  if (role === 'script') {
    // After the script, each name it declared becomes data (this.name).
    const keep = declaredNames(source)
      .map((name) => `if (typeof ${name} !== 'undefined') this.${name} = ${name};`)
      .join('\n');
    const run = scoped(`${source}\n;${keep}`);
    return function script(context) { run.call(this, scope(context, this, names)); };
  }
  if (role !== 'location') {
    // An expression written as a statement (`new testobject();`) still is one.
    const expression = source.trim().replace(/;+$/, '');
    const run = scoped(`return (${expression}\n);`);
    return function expression(context) { return run.call(this, scope(context, this, names)); };
  }
  const location = source.trim();
  if (IDENTIFIER.test(location)) {
    // A data name: napi's own locations assign only declared names.
    return location;
  }
  const read = scoped(`return (${location});`);
  const write = scoped(`(${location}) = __value;`, '__value');
  return {
    get(context) { return read.call(this, scope(context, this, names)); },
    set(value, context) { write.call(this, scope(context, this, names), value); },
  };
}

// ---------------------------------------------------------------------------
// print (napi ADR 0031)

const serialize = Symbol.for('tinyactors.serialize');
const IDENTIFIER_NAME = /^[A-Za-z_$][A-Za-z0-9_$]*$/;
const STANDARD = ['Date', 'Map', 'Set', 'RegExp', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
  'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array'];

// Refuses as a codec refusal. The package's ChartCodecError, when the
// package is where this file expects it; otherwise an Error, which encode()
// passes on as it is.
function refuse(message) {
  let ChartCodecError;
  try {
    ({ ChartCodecError } = require('..'));
  } catch {
    throw new Error(message);
  }
  throw new ChartCodecError('unrepresentable', message);
}

function number(value) {
  if (Object.is(value, -0)) {
    return '-0';
  }
  return Number.isNaN(value) ? 'NaN' : String(value);
}

function literal(value, seen) {
  switch (typeof value) {
    case 'undefined': return 'undefined';
    case 'boolean': return String(value);
    case 'number': return number(value);
    case 'bigint': return `${value}n`;
    case 'string': return JSON.stringify(value);
    case 'symbol': return refuse('a symbol has no text');
    case 'function':
      if (typeof value[serialize] !== 'function') {
        return refuse('a function as a value has no text; name it with registerFunction()');
      }
      break;
    default:
      if (value === null) {
        return 'null';
      }
  }
  if (seen.has(value)) {
    return refuse('a value with a cycle has no text');
  }
  seen.add(value);
  try {
    return object(value, seen);
  } finally {
    seen.delete(value);
  }
}

function construction(name, args, seen) {
  return `new ${name}(${args.map((item) => literal(item, seen)).join(', ')})`;
}

function object(value, seen) {
  if (typeof value[serialize] === 'function') {
    const description = value[serialize]();
    if (typeof description?.name !== 'string' || !IDENTIFIER_NAME.test(description.name) ||
        !Array.isArray(description.arguments)) {
      return refuse('[serialize]() must answer { name, arguments } with an identifier and an array');
    }
    return construction(description.name, description.arguments, seen);
  }
  const prototype = Object.getPrototypeOf(value);
  const standard = STANDARD.find((name) => globalThis[name].prototype === prototype);
  if (standard === 'Date') {
    return `new Date(${number(value.getTime())})`;
  }
  if (standard === 'Map') {
    return `new Map([${[...value].map(([key, item]) => `[${literal(key, seen)}, ${literal(item, seen)}]`).join(', ')}])`;
  }
  if (standard === 'Set') {
    return `new Set([${[...value].map((item) => literal(item, seen)).join(', ')}])`;
  }
  if (standard === 'RegExp') {
    return construction('RegExp', [value.source, value.flags], seen);
  }
  if (standard !== undefined) {
    return `new ${standard}([${Array.from(value, (item) => literal(item, seen)).join(', ')}])`;
  }
  const array = Array.isArray(value);
  if (!array && prototype !== Object.prototype && prototype !== null) {
    return refuse(`${value.constructor?.name ?? 'this object'} has no text; give it [serialize]() and register its class`);
  }
  if (Object.getOwnPropertySymbols(value).length !== 0) {
    return refuse('an object with symbol keys has no text');
  }
  const descriptors = Object.getOwnPropertyDescriptors(value);
  const keys = Object.keys(descriptors).filter((key) => !(array && key === 'length'));
  if (array && keys.length !== value.length) {
    return refuse('an array with holes has no text');
  }
  if (keys.some((key) => !descriptors[key].enumerable || !('value' in descriptors[key]))) {
    return refuse('an object with accessors or hidden properties has no text');
  }
  if (array) {
    return `[${value.map((item) => literal(item, seen)).join(', ')}]`;
  }
  // A key such as `__proto__` stays an own property: computed keys define it.
  return `{${keys.map((key) => `[${JSON.stringify(key)}]: ${literal(descriptors[key].value, seen)}`).join(', ')}}`;
}

// A function's source as an expression that calls it, or a refusal: native
// and bound functions, classes and methods have no source that is one.
function called(run) {
  const source = Function.prototype.toString.call(run);
  if (/\{\s*\[native code\]\s*\}\s*$/.test(source) || /^class\b/.test(source)) {
    return refuse('this function has no source text; name it with registerFunction()');
  }
  try {
    // Parses, never runs: a function made from the text is never called.
    new Function(`return (${source}\n);`);
  } catch {
    return refuse('this function\'s source is not an expression (a method?); name it with registerFunction()');
  }
  return `(${source}\n).call(this, __context)`;
}

function print(expression) {
  switch (expression.kind) {
    case 'function': return called(expression.function);
    case 'literal': return literal(expression.value, new Set());
    default: return refuse('a location has no text; use a data name');
  }
}

module.exports = { ecmascript: { compile, print } };
