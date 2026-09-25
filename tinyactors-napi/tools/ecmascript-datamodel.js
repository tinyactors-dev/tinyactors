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

// The system variables of one call, each read only when an expression uses it.
function scope(context) {
  let event;
  return {
    get _event() {
      if (event === undefined) {
        event = scxmlEvent(context.event);
      }
      return event;
    },
    get _sessionid() { return String(context.sessionID); },
    get _name() { return context.name; },
    get _ioprocessors() { return context.ioProcessors; },
    In(id) { return context.inState(id); },
  };
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

function compile(source, role) {
  if (!ROLES.has(role)) {
    throw new TypeError(`unknown role ${role}`);
  }
  try {
    return compileNow(source, role);
  } catch (error) {
    if (error instanceof SyntaxError) {
      return deferred(error, role);
    }
    throw error;
  }
}

function compileNow(source, role) {
  if (role === 'script') {
    const run = scoped(`${source}\n`);
    return function script(context) { run.call(this, scope(context)); };
  }
  if (role !== 'location') {
    const run = scoped(`return (${source}\n);`);
    return function expression(context) { return run.call(this, scope(context)); };
  }
  const location = source.trim();
  if (IDENTIFIER.test(location)) {
    // A data name: napi's own locations assign only declared names.
    return location;
  }
  const read = scoped(`return (${location});`);
  const write = scoped(`(${location}) = __value;`, '__value');
  return {
    get(context) { return read.call(this, scope(context)); },
    set(value, context) { write.call(this, scope(context), value); },
  };
}

module.exports = { ecmascript: { compile } };
