// The convenience layer of @tinyactors/node: JavaScript over the direct
// layer's exports only (AGENTS.md, "Layers and the resolver"). It adds
// shapes, not behaviour, and imports nothing but lib/direct.mjs, the
// package's direct exports (the lint rule convenience-imports-the-direct-layer).
//
// The typed methods as promises (docs/plan.xml, d-promises): beside each
// direct method `host.actorState(params, onResult, onError)` a method
// `host.actorStateAsync(params)` that calls it with a result handler that
// resolves and an error handler that rejects with the same
// TinyactorsError. The direct methods are left as they are, so a caller
// that passes handlers pays nothing for this module. The promise settles
// from the handler, in the drain; its continuation runs as a microtask
// after the turn's native call returns, outside the drain, on a result
// already converted. A refusal at the call (a closed host, a `this` that
// is not a Host) rejects too, as an async function's throw would. A
// method still pending at `close()` never settles: the direct layer drops
// its handlers uncalled.
import { Host } from './direct.mjs';

/** The direct method `direct` as a method of the same params returning a
 * promise of its result. */
function promised(direct) {
  return function (params) {
    return new Promise((resolve, reject) => {
      direct.call(this, params, (host, result) => resolve(result), (host, error) => reject(error));
    });
  };
}

/** Each direct typed method of Host, by the name of its promise form. */
const TYPED_METHODS = {
  actorStateAsync: Host.prototype.actorState,
  actorContextAsync: Host.prototype.actorContext,
  actorListAsync: Host.prototype.actorList,
  actorGetAsync: Host.prototype.actorGet,
  hostStatsAsync: Host.prototype.hostStats,
  hostConfigAsync: Host.prototype.hostConfig,
  traceConfigureAsync: Host.prototype.traceConfigure,
  traceWatchAsync: Host.prototype.traceWatch,
  traceUnwatchAsync: Host.prototype.traceUnwatch,
  traceEndAsync: Host.prototype.traceEnd,
  traceSpansDecodeAsync: Host.prototype.traceSpansDecode,
};

for (const [name, direct] of Object.entries(TYPED_METHODS)) {
  if (typeof direct !== 'function') throw new Error(`@tinyactors/node: the direct layer has no Host method for ${name}`);
  const method = promised(direct);
  Object.defineProperty(method, 'name', { value: name });
  // as a class's methods are: writable, configurable, not enumerable
  Object.defineProperty(Host.prototype, name, { value: method, writable: true, configurable: true, enumerable: false });
}
