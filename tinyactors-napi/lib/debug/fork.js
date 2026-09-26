'use strict';

// A fork: a copy of a live actor in a debug System of its own (the
// proposal's "A fork needs its own system"; core ADR 0085, napi ADR 0026).
//
// - The debug system has a manual clock that starts at the origin's time,
//   so `clock +3d` runs the future now without moving production's.
// - The actor's exact revision is registered there from its export
//   (spans included), since an image restores only against its revision.
// - The copy binds a structuredClone of the live data root, so stepping it
//   never changes production's data.
// - Every I/O processor and invoker type the chart names literally gets a
//   recording stand-in, so no step on the copy sends a real request; a
//   send to another actor of the origin fails in the copy as one to a
//   destroyed actor does.
// - Restoring an image turns each active invocation into
//   error.communication.invoke.<id>.unavailable, so the copy says how it
//   differs when it is made.
// - The copy is also the debugger's time machine: an image captured at a
//   stop restores it there (stepBack, reverseContinue, restartFrame).

const { core } = require('../native');
const { TinyactorsError } = require('../errors');
const { createSystem, nativeOf } = require('../system');

const SCXML_TYPES = new Set(['scxml', 'http://www.w3.org/TR/scxml/', 'http://www.w3.org/TR/scxml',
                             'http://www.w3.org/TR/scxml/#SCXMLEventProcessor']);

// The I/O processor and invoker types a spec names literally, and whether
// it names some by expression.
function servicesOf(spec) {
  const io = new Set();
  const invokers = new Set();
  let computed = false;
  const literal = (attribute, into) => {
    if (attribute === undefined) return;
    if (attribute.kind === 'literal') {
      if (!SCXML_TYPES.has(attribute.literal)) into.add(attribute.literal);
    } else {
      computed = true;
    }
  };
  const actions = (list) => {
    for (const action of list ?? []) {
      if (action.kind === 'send') literal(action.send?.type, io);
      if (action.kind === 'if') {
        actions(action.conditional?.then_actions);
        actions(action.conditional?.else_actions);
      }
      if (action.kind === 'foreach') actions(action.foreach?.body);
    }
  };
  const states = (list) => {
    for (const state of list ?? []) {
      actions(state.initial?.actions);
      for (const handler of [...(state.onentry ?? []), ...(state.onexit ?? [])]) actions(handler.actions);
      for (const transition of state.transitions ?? []) actions(transition.actions);
      for (const invoke of state.invokes ?? []) {
        literal(invoke.type, invokers);
        actions(invoke.finalize);
      }
      states(state.states);
    }
  };
  actions(spec.initial?.actions);
  states(spec.states);
  return { io: [...io], invokers: [...invokers], computed };
}

class Fork {
  #origin;
  #actor;
  #record;
  #definitions = new Map();
  #notes = [];

  /**
   * Copies `actor` of `origin` into a new debug system. `record(text)`
   * hears what the stand-ins would have done; `log(record)` gets the
   * copy's <log> records.
   */
  constructor(origin, actor, { record, log }) {
    this.#origin = origin;
    this.#actor = actor;
    this.#record = record;
    const view = origin.inspect(actor);
    this.system = createSystem({ clock: 'manual', initialTime: origin.time, finished: 'retain', log });
    try {
      this.#define(view.definitionID);
      const active = view.invocations.filter((invocation) => invocation.active).length;
      if (active > 0) {
        this.#notes.push(`its ${active} active invocation${active === 1 ? ' is' : 's are'} unavailable in the copy: ` +
                         'it gets error.communication.invoke.<id>.unavailable');
      }
      this.copy = this.#copy();
    } catch (error) {
      this.system.close();
      throw error;
    }
  }

  /** What makes the copy differ from the live actor, as sentences. */
  get notes() {
    return [...this.#notes];
  }

  /** The debug system's Definition of the origin definition `id`. */
  #define(id) {
    const key = String(id);
    let definition = this.#definitions.get(key);
    if (definition !== undefined) return definition;
    const spec = core.definition_export(nativeOf(this.#origin), id);
    const services = servicesOf(spec);
    for (const type of services.io) {
      this.system.registerIO(type, {
        send: (request) => this.#record(`would send ${request.event ?? '(no event)'} via ${type} to ` +
                                        `${typeof request.target === 'string' ? request.target : JSON.stringify(request.target)}`),
      });
    }
    for (const type of services.invokers) {
      this.system.registerInvoker(type, {
        start: (request) => {
          this.#record(`would start invocation ${request.invokeID ?? ''} of type ${type}`);
          return { cancel: () => this.#record(`would cancel invocation ${request.invokeID ?? ''}`) };
        },
      });
    }
    if (services.io.length > 0 || services.invokers.length > 0) {
      this.#notes.push(`its I/O processors and invokers (${[...services.io, ...services.invokers].join(', ')}) are ` +
                       'stand-ins that only record');
    }
    if (services.computed) {
      this.#notes.push('a send or invocation whose type is an expression reaches no processor in the copy');
    }
    this.#notes.push('a send to another actor of the live system fails in the copy, as to a destroyed actor');
    definition = this.system.define(spec);
    this.#definitions.set(key, definition);
    return definition;
  }

  // A new copy of the live actor, bound to a copy of its data.
  #copy() {
    const image = this.#origin.capture(this.#actor);
    try {
      return this.system.reconstruct(image, { binding: cloneData(image.data) });
    } finally {
      image.destroy();
    }
  }

  /** Replaces the copy with a new one of the live actor (restart). */
  renew() {
    if (this.system.exists(this.copy)) this.system.destroy(this.copy);
    this.copy = this.#copy();
    return this.copy;
  }

  /** An image of the copy as it stands, with a copy of its data: a place to come back to. */
  capture() {
    const image = this.system.capture(this.copy);
    try {
      return { image, data: cloneData(image.data) };
    } catch (error) {
      image.destroy();
      throw error;
    }
  }

  /** Puts the copy back where `point` (from capture()) found it. */
  restore(point) {
    if (this.system.exists(this.copy)) this.system.destroy(this.copy);
    this.copy = this.system.reconstruct(point.image, { binding: cloneData(point.data) });
    return this.copy;
  }

  close() {
    this.system.close();
  }
}

function cloneData(data) {
  if (data === undefined) return undefined;
  try {
    return structuredClone(data);
  } catch (error) {
    throw new TinyactorsError(`the actor's data cannot be copied for a fork: ${error.message}`, 'invalid');
  }
}

module.exports = { Fork, servicesOf };
