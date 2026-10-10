/**
 * tinyactors for browsers — the same API as the Node.js binding
 * (bindings/shared/core.mjs), over the tinyactors runtime compiled to
 * WebAssembly (tinyactors.wasm).
 *
 *   import { scxml, createSystem, createActor } from './bindings/web/index.mjs';
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
    repeat: (fn, ms) => setInterval(fn, ms),
    clearRepeat: (h) => clearInterval(h),
    log: (...args) => console.log(...args),
  },
});

export const {
  scxml, fromSCXML, Chart, createSystem, System, createActor, Actor, ActorRef, toPromise, loadImage, Image,
  createLink, serveLinks, createStoreLink, parseAddress, formatAddress, encodeFrame, FrameDecoder,
  parseTraceparent, formatTraceparent, stepSpanID, decodeTraces, metricsToOTLP, TRACE_KINDS,
} = api;
export default api;
