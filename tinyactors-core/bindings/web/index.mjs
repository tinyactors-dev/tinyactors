/**
 * tinyactors for browsers — the same xstate-style API as the Node.js binding
 * (bindings/shared/core.mjs), over the tinyactors runtime compiled to
 * WebAssembly (tinyactors.wasm, ~50 KB gzipped). The datamodel's values are
 * plain JavaScript values; machines compile to readable SCXML in the browser.
 *
 *   import { createMachine, createActor, createLink } from './bindings/web/index.mjs';
 */
import { createRuntime } from '../shared/core.mjs';
import { createNative } from './native.mjs';

const url = new URL('./tinyactors.wasm', import.meta.url);
const native = await createNative(async (imports) => {
  const response = fetch(url);
  if (WebAssembly.instantiateStreaming) {
    try {
      return (await WebAssembly.instantiateStreaming(response, imports)).instance;
    } catch {
      /* e.g. served without application/wasm: fall back to bytes */
    }
  }
  const bytes = await (await fetch(url)).arrayBuffer();
  return (await WebAssembly.instantiate(bytes, imports)).instance;
});

/* setImmediate for browsers: a MessageChannel turn (not clamped like setTimeout) */
const channel = new MessageChannel();
const deferred = new Map();
let deferSeq = 0;
channel.port1.onmessage = (e) => {
  const fn = deferred.get(e.data);
  deferred.delete(e.data);
  if (fn) fn();
};

const api = createRuntime({
  native,
  platform: {
    now: () => performance.now(),
    defer: (fn) => {
      const id = ++deferSeq;
      deferred.set(id, fn);
      channel.port2.postMessage(id);
      return id;
    },
    cancelDefer: (id) => deferred.delete(id),
    setTimer: (fn, ms) => setTimeout(fn, ms),
    clearTimer: (h) => clearTimeout(h),
    log: (...args) => console.log(...args),
  },
});

export const {
  createMachine, createActor, createSystem, fromSCXML, SCXMLLogic, compileSystem, loadImage, SystemImage, setup, assign, raise, sendTo, sendParent, log, cancel,
  stateIn, and, or, not, StateMachine, Actor, System, ActorRef, createLink, serveLinks, encodeFrame, FrameDecoder,
  createMemoryStore, encodeValue, decodeValue, parseAddress, formatAddress, encodeWire, decodeWire, createStoreLink,
  TRACE_KINDS, decodeTraces, parseTraceparent, formatTraceparent, stepSpanID,
} = api;
export default api;
