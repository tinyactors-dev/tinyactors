// readSnapshot, as the addon defines it (src/snapshot.c): a snapshot read
// without a host (tinyactors-core/docs/embedding.md, section 12), over the
// module's wasm/src/snapshot.c. The module keeps the pack's copy and each
// segment's region read with ta_system_read; a Snapshot holds its address
// in a private field and frees it when it is collected (a
// FinalizationRegistry, as a compile result is freed). Each method is one
// query, answered as CBOR in the environment's arena and read before the
// call leaves; what is an id or a time is a bigint, as the addon's.
import { bytesOf } from './checks.mjs';
import * as cbor from './cbor.mjs';
import { refusal } from './errors.mjs';
import { instance } from './state.mjs';

const QUERY = Object.freeze({
  actors: 0, actor: 1, configuration: 2, mailbox: 3, timers: 4, stats: 5, charts: 6, traceKept: 7, traces: 8,
});
const TA_ERROR_INVALID = -3;
const TA_ERROR_STALE = -5;

const unfreed = new FinalizationRegistry(({ module, snapshot }) => {
  try {
    if (!module.poisoned) {
      module.setWord(0, snapshot);
      module.call('ta_web_snapshot_free', module.env);
    }
  } catch {
    // a poisoned module frees nothing more
  }
});

// The token the constructor checks: only readSnapshot makes a Snapshot.
const MAKING = Symbol('making');

const big = (value) => (value === null || value === undefined ? value : BigInt(value));

export class Snapshot {
  #module;
  #snapshot;

  constructor(making, module, snapshot, header) {
    if (making !== MAKING) throw new TypeError('a Snapshot is made by readSnapshot');
    this.#module = module;
    this.#snapshot = snapshot;
    Object.defineProperties(this, {
      systemID: { value: header.systemID, enumerable: true },
      datamodel: { value: header.datamodel, enumerable: true },
      time: { value: header.time, enumerable: true },
      segments: { value: header.segments, enumerable: true },
      state: { value: header.state, enumerable: true },
    });
    unfreed.register(this, { module, snapshot });
  }

  // One query of the module: its answer, read from CBOR.
  static #query(self, kind, segment, handle) {
    if (!(#snapshot in self)) throw new TypeError('not a Snapshot');
    const module = self.#module;
    const scratch = module.scratch;
    return module.scoped(scratch, () => {
      module.setWord(0, self.#snapshot);
      module.setWord(1, Number.isInteger(segment) && segment >= 0 && segment <= 0xffffffff ? segment : 0xffffffff);
      module.setWord(2, Number.isInteger(handle) && handle >= 0 && handle <= 0xffffffff ? handle : 0);
      const status = module.call('ta_web_snapshot_query', module.env, kind);
      if (status === TA_ERROR_INVALID) throw new RangeError('no live segment of that index in the snapshot');
      if (status === TA_ERROR_STALE) throw new RangeError('no actor of that handle in the segment');
      if (status !== 0) throw refusal(module.statusName(status), 'the snapshot could not be read');
      return cbor.read(module, scratch, module.word(0), module.word(1), { addresses: false });
    });
  }

  actors(segment) { return Snapshot.#query(this, QUERY.actors, segment); }

  actor(segment, handle) {
    const actor = Snapshot.#query(this, QUERY.actor, segment, handle);
    return { ...actor, session: big(actor.session), parent: big(actor.parent) };
  }

  configuration(segment, handle) { return Snapshot.#query(this, QUERY.configuration, segment, handle); }

  mailbox(segment, handle) { return Snapshot.#query(this, QUERY.mailbox, segment, handle); }

  timers(segment, handle) {
    return Snapshot.#query(this, QUERY.timers, segment, handle).map((timer) => ({ ...timer, due: big(timer.due) }));
  }

  stats(segment) {
    const stats = Snapshot.#query(this, QUERY.stats, segment);
    return { ...stats, now: big(stats.now), nextDeadline: big(stats.nextDeadline), wallNanoseconds: big(stats.wallNanoseconds) };
  }

  charts(segment) { return Snapshot.#query(this, QUERY.charts, segment); }

  traceKept(segment) { return Snapshot.#query(this, QUERY.traceKept, segment); }

  traces(segment) { return Snapshot.#query(this, QUERY.traces, segment); }
}

// readSnapshot(bytes) -> Snapshot: a pack of this build, copied during the
// call.
export function readSnapshot(bytes) {
  const pack = bytesOf(bytes, 'readSnapshot takes a Uint8Array or an ArrayBuffer');
  const module = instance();
  const scratch = module.scratch;
  return module.scoped(scratch, () => {
    const [at, length] = module.copyIn(scratch, pack);
    module.setWord(0, at);
    module.setWord(1, length);
    const status = module.call('ta_web_snapshot_read', module.env);
    if (status !== 0) {
      throw refusal(module.statusName(status), status === TA_ERROR_INVALID
        ? 'not a snapshot of this build (another format or layout, or damaged)'
        : 'no memory to read the snapshot');
    }
    const header = {
      systemID: module.string(module.word(1), module.word(2)),
      datamodel: module.string(module.word(3), module.word(4)),
      time: module.word64(5),
      segments: module.word(7),
      state: module.copyOut(module.word(8), module.word(9)),
    };
    return new Snapshot(MAKING, module, module.word(0), header);
  });
}
