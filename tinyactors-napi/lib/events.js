'use strict';

// Event-name cache: text -> ta_event_name_id (ADR 0004).
//
// One cache per system. A miss makes a single native call that validates and
// interns the dotted name; a name seen before does no string work at all, and
// native post paths take the numeric ID. Core interning is monotonic, so an
// entry never goes stale; a handle to a name core keeps only as text
// (TA_EVENT_NAME_TEXT) is never cached. The cache grows with the number of distinct names
// the application uses, not with traffic.

const { TinyactorsError } = require('./errors');

// TA_EVENT_NAME_TEXT: the bit of a name ID that is a handle to text an
// input keeps, not an interned name.
const TEXT_HANDLE = 0x80000000;

class EventNames {
  #native;
  #ids = new Map();
  #names = new Map();

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

  /**
   * The dotted text of a name core reports (inspections, events), formatted
   * once per ID and cached (ADR 0004 point 3).
   */
  name(id) {
    if ((id & TEXT_HANDLE) !== 0) {
      // A name core keeps only as text: the ID is a handle valid while its
      // event is, so it is read now and never cached (core ADR 0100).
      return this.#native.eventName(id);
    }
    let text = this.#names.get(id);
    if (text === undefined) {
      text = this.#native.eventName(id);
      this.#names.set(id, text);
      this.#ids.set(text, id);
    }
    return text;
  }

  /** Distinct names interned through this cache. */
  get size() {
    return this.#ids.size;
  }
}

module.exports = { EventNames };
