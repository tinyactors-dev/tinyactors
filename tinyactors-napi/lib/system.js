'use strict';

// System facade over the native System (ADR 0012). Later steps add the
// generated core module and the plumbing behind it.

const native = require('./native');
const { TinyactorsError } = require('./errors');

class System {
  #native;

  constructor(options) {
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TinyactorsError('createSystem options must be an object', 'invalid');
    }
    this.#native = new native.System();
  }

  /** True once close() has run. */
  get closed() {
    return this.#native.closed;
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

function createSystem(options) {
  return new System(options);
}

module.exports = { System, createSystem };
