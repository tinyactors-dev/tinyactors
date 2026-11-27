// readSnapshot, as the addon defines it (src/snapshot.c): a snapshot read
// without a host, over core's snapshot reader
// (tinyactors-core/docs/snapshot-requests.md) in the module's
// wasm/src/snapshot.c. The module keeps the reader's block; the reader
// holds its address in a private field and frees it when it is collected
// (a FinalizationRegistry, as a compile result is freed). Its one method is
// request(method, params): the message written as CBOR in the environment's
// arena, the reply read from it before the call leaves. The convenience
// layer's Snapshot mirrors core's readers over it.
import { bytesOf } from './checks.mjs';
import * as cbor from './cbor.mjs';
import { coreError, refusal } from './errors.mjs';
import { instance } from './state.mjs';

const TA_ERROR_INVALID = -3;

const unfreed = new FinalizationRegistry(({ module, block }) => {
  try {
    if (!module.poisoned) {
      module.setWord(0, block);
      module.call('ta_web_snapshot_free', module.env);
    }
  } catch {
    // a poisoned module frees nothing more
  }
});

// The token the constructor checks: only readSnapshot makes a reader.
const MAKING = Symbol('making');

export class SnapshotReader {
  #module;
  #block;

  constructor(making, module, block, header) {
    if (making !== MAKING) throw new TypeError('a Snapshot is made by readSnapshot');
    this.#module = module;
    this.#block = block;
    Object.defineProperties(this, {
      systemID: { value: header.systemID, enumerable: true },
      datamodel: { value: header.datamodel, enumerable: true },
      time: { value: header.time, enumerable: true },
      segments: { value: header.segments, enumerable: true },
      state: { value: header.state, enumerable: true },
    });
    unfreed.register(this, { module, block });
  }

  // The reader's answer to one of its methods, or its error as a TinyactorsError.
  request(method, params) {
    if (!(#block in this)) throw new TypeError('not a Snapshot');
    if (typeof method !== 'string') throw new TypeError("request takes a method's name: a string");
    const module = this.#module;
    const scratch = module.scratch;
    return module.scoped(scratch, () => {
      const message = params === undefined || params === null ? { id: 1, method } : { id: 1, method, params };
      const [at, length] = cbor.write(module, scratch, message);
      module.setWord(0, this.#block);
      module.setWord(1, at);
      module.setWord(2, length);
      const status = module.call('ta_web_snapshot_request', module.env);
      if (status !== 0) throw refusal(module.statusName(status), 'the snapshot request could not be answered');
      const reply = cbor.read(module, scratch, module.word(0), module.word(1), { addresses: false });
      if ('result' in reply) return reply.result;
      throw coreError(reply.error);
    });
  }
}

// readSnapshot(bytes) -> SnapshotReader: a pack of this build, copied
// during the call.
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
    return new SnapshotReader(MAKING, module, module.word(0), header);
  });
}
