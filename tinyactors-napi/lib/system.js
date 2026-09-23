'use strict';

// System facade over the native System (ADR 0012) and the generated core
// module (ADR 0013). Scheduling is native (ADR 0007): nothing here takes part
// in it; spawn() and destroy() only reach core, whose hooks tell the pump.

const native = require('./native');
const { TinyactorsError } = require('./errors');
const { Definition, identityOf } = require('./definition');
const { DefinitionBuilder } = require('./statechart');
const { ActorId } = require('./actor');
const { EventNames } = require('./events');

const NO_STATE = 4294967295;

const { core } = native;

// Compiles a spec in the generated shape. A refused chart throws with core's
// compile error formatted into the message (ADR 0058), and the error itself,
// with a readable path, as `detail`.
function compile(system, spec) {
  try {
    return core.definition_compile_spec(system, spec);
  } catch (error) {
    const refusal = error instanceof TinyactorsError ? error.detail?.error : undefined;
    if (refusal === undefined) {
      throw error;
    }
    let text;
    try {
      text = core.compile_error_format(spec, refusal);
    } catch {
      text = `${refusal.code}: ${refusal.message}`;
    }
    throw new TinyactorsError(`chart refused: ${text}`, error.code, {
      error: refusal,
      code: refusal.code,
      path: pathText(refusal.path),
    });
  }
}

// `states[0].transitions[1].condition`, from core's path elements.
function pathText(path) {
  const noIndex = 4294967295;
  return (path ?? [])
    .map(({ member, index }, at) => `${at === 0 ? '' : '.'}${member}${index === noIndex ? '' : `[${index}]`}`)
    .join('');
}

// The native System behind a public one, for other lib/ modules and tests.
// index.js does not export it.
let nativeOf;

class System {
  #native;
  #events;

  static {
    nativeOf = (system) => system.#native;
  }

  /**
   * Options:
   * - scheduling: { resumeBudget, actionBudget, resumesPerTurn } (ADR 0007);
   * - finished: "destroy" (the default) or "retain" (ADR 0012);
   * - scheduling.messageBudget: deliveries per turn, and dead letters
   *   reported per turn; scheduling.maxQueuedMessages: post() throws
   *   `queue-full` beyond it (ADR 0005);
   * - done({ actor, finalState, data }), fault({ actor, operation, code,
   *   message }) and deadLetter({ target, event, reason }): hooks the pump
   *   calls at the end of a turn (ADRs 0008, 0012).
   */
  constructor(options) {
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TinyactorsError('createSystem options must be an object', 'invalid');
    }
    this.#native = new native.System(this, options);
    this.#events = new EventNames(this.#native);
  }

  /** True once close() has run. */
  get closed() {
    return this.#native.closed;
  }

  /**
   * Compile and register a chart: a statechart() builder, or a spec in the
   * generated shape (the shape core's definition export returns). Until the
   * JavaScript data model lands (ROADMAP step 11), charts compile against
   * core's null data model.
   */
  define(source) {
    let spec = source;
    if (source instanceof DefinitionBuilder) {
      spec = source.build();
    } else if (source === null || typeof source !== 'object') {
      throw new TinyactorsError('define(): expected a statechart() builder or a spec object', 'invalid');
    }
    const compiled = compile(this.#native, spec);
    const id = core.system_register_definition(this.#native, compiled.out);
    return new Definition(this, this.#native, id);
  }

  /**
   * The newest registered, unretired definition with this identity, or
   * undefined.
   */
  findDefinition(identity) {
    if (identity === null || typeof identity !== 'object') {
      throw new TinyactorsError('findDefinition(): expected { family, revision }', 'invalid');
    }
    try {
      const id = core.system_find_definition(this.#native, {
        family: { name: identity.family },
        revision: identity.revision,
      });
      return new Definition(this, this.#native, id);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return undefined;
      }
      throw error;
    }
  }

  /**
   * Create an actor of `definition`. It is bootstrapped by a later pump turn,
   * never inside this call (ADR 0007); `await system.flush()` to see it in its
   * initial state.
   */
  spawn(definition, options = {}) {
    if (!(definition instanceof Definition) || definition.system !== this) {
      throw new TinyactorsError('spawn(): expected a Definition of this system', 'invalid');
    }
    let address;
    if (options.address !== undefined) {
      address = { family: { name: options.address.family }, name: options.address.name };
    } else if (options.name !== undefined) {
      address = { family: { name: definition.identity.family }, name: options.name };
    }
    const id = core.actor_spawn(this.#native, definition.id, address, options.binding);
    return new ActorId(this, id.slot, id.generation);
  }

  /** A snapshot of core's view of `actor` (ADR 0054: O(size of the actor)). */
  inspect(actor) {
    const { slot, generation } = actorOf(this, actor);
    const view = core.actor_inspect(this.#native, slot, generation);
    const stateName = (state) => core.definition_state(this.#native, view.definition_id, state).id;
    const inspection = {
      actor: new ActorId(this, view.actor.slot, view.actor.generation),
      sessionId: view.session_id,
      definition: identityOf(view.definition),
      residency: view.residency,
      published: view.published,
      activeStates: core.actor_active_states(this.#native, slot, generation).map(stateName),
      macrostepInProgress: view.macrostep_in_progress,
      internalEventCount: view.internal_events.length,
      delayedSendCount: view.delayed_sends.length,
      scheduling: this.#native.scheduling(slot, generation),
      // The top-level final it reached, or null (core ADR 0056).
      finalState: view.final_state === NO_STATE ? null : stateName(view.final_state),
      allocatedBytes: view.allocated_bytes,
    };
    return inspection;
  }

  /** True when a resident actor has this ID. */
  exists(actor) {
    const { slot, generation } = actorOf(this, actor);
    try {
      core.actor_inspect(this.#native, slot, generation);
      return true;
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return false;
      }
      throw error;
    }
  }

  /** Terminate `actor`. Throws `not-found` for an actor that is gone. */
  destroy(actor) {
    const { slot, generation } = actorOf(this, actor);
    core.actor_destroy(this.#native, slot, generation);
  }

  /**
   * Queue `event` for `target` and return (ADR 0005). Nothing runs inside
   * this call: a later pump turn delivers it, in the order posts to that
   * target were accepted. Mail for an actor that is gone or finished is a
   * dead letter (the `deadLetter` hook), not an error. Throws `queue-full`
   * at scheduling.maxQueuedMessages. Event data needs the JavaScript data
   * model (ROADMAP step 11); named targets need the directory (step 7).
   */
  post(target, event, data, options) {
    if (data !== undefined) {
      throw new TinyactorsError('post(): event data needs the JavaScript data model (ROADMAP step 11)', 'unsupported');
    }
    if (options?.signal?.aborted) {
      throw new TinyactorsError('post(): the signal is already aborted', 'aborted');
    }
    if (!(target instanceof ActorId)) {
      if (target !== null && typeof target === 'object' && 'name' in target) {
        throw new TinyactorsError('post(): named targets need the actor directory (ROADMAP step 7)', 'unsupported');
      }
      throw new TinyactorsError('post(): expected an actor of this system', 'invalid');
    }
    const { slot, generation } = actorOf(this, target);
    this.#native.post(slot, generation, this.#events.id(event));
  }

  /** Put an actor that faulted back into scheduling (ADR 0008). */
  release(actor) {
    const { slot, generation } = actorOf(this, actor);
    this.#native.release(slot, generation);
  }

  /**
   * Resolves once the system is quiet: nothing runnable, nothing ready, no
   * dead letters to report (later: nothing due). It spans as many pump turns as needed and yields to
   * the event loop between them. close() rejects it with `closed`.
   */
  flush() {
    return this.#native.flush();
  }

  /**
   * Memory counters by category and the number of allocator calls
   * (ADRs 0009 and 0014). Throws `closed` after close().
   */
  stats() {
    return this.#native.stats();
  }

  /**
   * Destroy the native system and everything it holds. Idempotent and
   * authoritative: afterwards every method except close() throws `closed`.
   */
  close() {
    this.#native.close();
  }

  [Symbol.dispose]() {
    this.close();
  }
}

// The (slot, generation) of an ActorId of `system` (ADR 0002).
function actorOf(system, actor) {
  if (!(actor instanceof ActorId) || actor.system !== system) {
    throw new TinyactorsError('expected an actor of this system', 'invalid');
  }
  return actor;
}

function createSystem(options) {
  return new System(options);
}

module.exports = { System, createSystem, nativeOf, ActorId };
