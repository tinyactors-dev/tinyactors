# SCXML documents

`define()` imports an SCXML document as well as a `statechart()` builder
(napi ADR 0019, core ADRs 0053 and 0076):

```js
const { createSystem } = require('tinyactors-napi');
const system = createSystem();
const definition = system.define(xmlText, { format: 'scxml' }); // a string or a Uint8Array of UTF-8
```

The document's `datamodel` attribute picks the data model:

- no attribute, or `null`: core's null data model, which runs `In()` and
  nothing else;
- `javascript`: napi's own data model, whose source text is only
  `In(state)` and bare data names;
- any other name: a data model registered with `registerDatamodel()`.

## A data model for expression text

tinyactors ships no expression compiler, because running expression text
means running code with your process's rights. You register one:

```js
system.registerDatamodel('ecmascript', {
  compile(source, role) {
    // role: 'value' | 'condition' | 'location' | 'script'
    // value, condition, script: return function (context) { ... } with `this` = the data
    // location: return a data name, or { get(context), set(value, context) }
  },
});
```

`compile` runs once per expression when the document is defined. What it
returns runs on napi's built-in data model, exactly as a builder chart's
functions do, so data, `<foreach>`, `<param>`, `<content>` and `_event.data`
behave the same. The context is the one builder functions get: `event`,
`sessionID`, `name`, `ioProcessors` and `inState(id)`.

`tools/ecmascript-datamodel.js` in this repository is a complete example for
SCXML's ECMAScript data model. It compiles with `new Function` and `with`, and
maps `_event`, `_sessionid`, `_name`, `_ioprocessors` and `In()` onto the
context. It is not part of the package: use it, or write your own, for
documents you trust. For untrusted documents, compile in a sandbox, such as
an interpreter compiled to WebAssembly; `node:vm` is not a security boundary.

Rules for compiled functions, as for builder functions:

- they may read and change data, and `post()`;
- they may not call anything else on the system: that throws `reentrant`;
- a condition or a value may be evaluated again when a step stops for lack
  of memory (core ADR 0070), so conditions and values should have no side
  effects.

## `<data src>` and `<script src>`

The codec fetches nothing itself (core ADR 0053). Pass `resolve` and it is
called, once per `src`, while the document is defined:

```js
const definition = system.define(xmlText, {
  format: 'scxml',
  resolve: (src) => files.get(src), // a string, a Uint8Array, or undefined
});
```

A string or a `Uint8Array` is the content. `undefined` means the src is
unknown, and the document is refused at the attribute; so is any `src`
when there is no `resolve`. What `resolve` throws comes out of `define()`.

Content, whether fetched or written inline, that is JSON becomes that
value; any other content becomes a space-normalized string, as SCXML B.2.2
says: runs of whitespace are one space, with none at either end.

## `<invoke type="scxml">`: child documents

An invoke of type `scxml` (or `http://www.w3.org/TR/scxml/`) starts a
child session in the same system (napi ADR 0021). The child is an ordinary
actor with a parent link, so `#_parent`, `#_<invokeid>`, `done.invoke.<id>`,
`<donedata>`, `<finalize>` and `autoforward` work as SCXML says. Its
document is:

- a literal `<content>` holding an `<scxml>` document, imported when the
  invoke starts;
- the document a `src` or `srcexpr` names, fetched with the `resolve` the
  parent's document was defined with. A child imported that way keeps the
  same `resolve` for its own `src`s and `<data src>`. Without `resolve`, a
  `src` raises `error.execution` in the parent.

The child picks its data model with its own `datamodel` attribute. Each
distinct document is imported once per system and reused. Only the
`<param>` and `namelist` names the child declares as top-level `<data>`
enter its data (SCXML 6.4). A child document that cannot be imported
raises `error.execution` in the parent, whose data is the reason.

Children are reachable only through their parent (`#_<invokeid>`) or their
session ID; they have no name in the directory. Leaving the invoking state,
or destroying the parent, destroys the child. A child that faults, a
runaway for example, is quarantined like any actor, and its parent is not
told.

## When a document is refused

`define()` throws a `TinyactorsError` (`invalid`, or `unsupported` for a
data model nobody registered) whose `detail` says where:

```js
{ stage: 'compile', message: 'states[0].transitions[0].condition: ...', line: 4, column: 32, offset: 118 }
```

`stage` is `decode` when the document is not SCXML the codec reads, and
`compile` when core refused the chart. A `compile` that throws an error
with numeric `offset` and `length` points the position into the expression.

## Conformance

`mise run irp` runs the W3C SCXML IRP suite against the addon with the
example compiler, on Node and Bun. `test/irp/expected-failures.json` lists
the tests known to fail and why.
