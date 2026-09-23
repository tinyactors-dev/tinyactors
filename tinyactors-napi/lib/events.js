'use strict';

// Event-name cache: text -> ta_event_name_id (ADR 0004).
//
// One cache per system. A miss makes a single native call that validates and
// interns the dotted name; a name seen before does no string work at all, and
// native post paths take the numeric ID. Core interning is monotonic, so an
// entry never goes stale. The cache grows with the number of distinct names
// the application uses, not with traffic.

const { TinyactorsError } = require('./errors');

class EventNames {
  #native;
  #ids = new Map();

  constructor(native) {
    this.#native = native;
  }

  /** The ta_event_name_id of `name`, interning it on first use. */
  id(name) {
    let id = this.#ids.get(name);
    if (id === undefined) {
      if (typeof name !== 'string') {
        throw new TinyactorsError('an event name must be a string', 'invalid');
      }
      id = this.#native.internEvent(name);
      this.#ids.set(name, id);
    }
    return id;
  }

  /** Distinct names interned through this cache. */
  get size() {
    return this.#ids.size;
  }
}

module.exports = { EventNames };
