'use strict';
/**
 * tinyactors for Node.js — the API of core.mjs over the native addon
 * (src/addon.c and src/binding.c, against Node-API).
 */
const path = require('path');
const { createRuntime } = require('./core.mjs');
const { httpPost } = require('./transports');
const { telemetryFromEnvironment } = require('./telemetry');

/* The addon: an override, then this tree's build (mise run node:build),
 * then the platform package an install of @tinyactors/node brings
 * (@tinyactors/node-<platform>-<arch>, an optional dependency npm, pnpm and
 * Bun install only where it matches). The platform package is tried last,
 * so its error is the one an install without a matching addon reports. */
function loadNative() {
  const candidates = [
    process.env.TINYACTORS_NATIVE,
    path.join(__dirname, 'build', 'tinyactors.node'),
    `@tinyactors/node-${process.platform}-${process.arch}`,
  ].filter(Boolean);
  let lastError;
  for (const p of candidates) {
    try {
      return require(p);
    } catch (e) {
      lastError = e;
    }
  }
  throw lastError;
}

const platform = {
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
  /* traces go to OTEL_EXPORTER_OTLP_ENDPOINT with no code (createSystem({ telemetry }) wins) */
  telemetry: telemetryFromEnvironment(process.env),
  post: httpPost,
};

/* links in Node also over WebSocket servers ({ server, path } or { port })
 * and TCP ({ tcp }): transports.js */
const api = require('./transports').withNodeTransports(createRuntime({ native: loadNative(), platform }));

module.exports = api;
