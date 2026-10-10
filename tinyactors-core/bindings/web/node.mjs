/**
 * The web binding in Node.js: the same wasm module and JavaScript API as in
 * browsers, instantiated synchronously (so `require()` works) — used to run
 * the binding test suite against the wasm backend, and for server-side use
 * without a native addon. serveLinks() also accepts { server | port } and
 * { tcp } here, createLink() { tcp }.
 */
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { createRuntime } from '../shared/core.mjs';
import { createNative } from './native.mjs';

const bytes = readFileSync(new URL('./tinyactors.wasm', import.meta.url));
const { httpPost, withNodeTransports } = createRequire(import.meta.url)('../node/transports.js');
const native = createNative((imports) => new WebAssembly.Instance(new WebAssembly.Module(bytes), imports));

const api = createRuntime({
  native,
  platform: {
    now: () => performance.now(),
    defer: (fn) => setImmediate(fn),
    cancelDefer: (h) => clearImmediate(h),
    setTimer: (fn, ms) => setTimeout(fn, ms),
    clearTimer: (h) => clearTimeout(h),
    /* periodic work that must not keep the process alive (metrics export) */
    repeat: (fn, ms) => {
      const handle = setInterval(fn, ms);
      if (handle.unref) handle.unref();
      return handle;
    },
    clearRepeat: (h) => clearInterval(h),
    log: (...args) => console.log(...args),
    post: httpPost, /* (no environment is read: the application passes an endpoint) */
  },
});

withNodeTransports(api);

export const {
  scxml, fromSCXML, Chart, createSystem, System, createActor, Actor, ActorRef, toPromise, loadImage, Image,
  createLink, serveLinks, createStoreLink, parseAddress, formatAddress, encodeFrame, FrameDecoder,
  parseTraceparent, formatTraceparent, stepSpanID, decodeTraces, metricsToOTLP, TRACE_KINDS,
} = api;
export default api;
