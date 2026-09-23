'use strict';

// ActorId/Actor view (ADR 0002). An actor is named by its slot and
// generation, two small integers. A view holds its system and those two
// numbers and nothing else: it has no native resource and no finalizer, and
// every question about the actor is asked of core through the system. A view
// of a destroyed actor is harmless: its calls fail with `not-found`.
//
// The pump makes views natively for the actors it passes to hooks, so the
// class is registered with the addon once (setupActor).

const native = require('./native');

class ActorId {
  constructor(system, slot, generation) {
    this.system = system;
    this.slot = slot;
    this.generation = generation;
  }

  /** Equality is by value (ADR 0002 point 5). */
  equals(other) {
    return (
      other instanceof ActorId &&
      other.system === this.system &&
      other.slot === this.slot &&
      other.generation === this.generation
    );
  }

  /** A snapshot of core's view of this actor. */
  inspect() {
    return this.system.inspect(this);
  }

  /** True when no resident actor has this ID any more. */
  get destroyed() {
    return !this.system.exists(this);
  }

  /** Terminate this actor. Invoked children are not destroyed implicitly. */
  destroy() {
    this.system.destroy(this);
  }

  [Symbol.dispose]() {
    if (!this.system.closed && !this.destroyed) {
      this.destroy();
    }
  }
}

native.setupActor(ActorId);

module.exports = { ActorId };
