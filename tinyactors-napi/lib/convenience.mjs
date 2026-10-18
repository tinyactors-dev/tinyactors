// The convenience layer of @tinyactors/node and @tinyactors/wasm:
// JavaScript over the direct layer's exports only (AGENTS.md, "Layers and
// the resolver"). It adds shapes, not behaviour, and imports nothing but
// `#direct`, the package's direct exports (the lint rule
// convenience-imports-the-direct-layer): package.json's `imports` maps it
// to the addon's lib/direct.mjs, and under the export condition
// `tinyactors-wasm` to the wasm package's wasm/lib/direct.mjs
// (docs/browser.md, "One API"), so the two packages share this file.
//
// Then the shapes chosen for p3-shapes: host.addCharts, host.spawnChart,
// directoryResolver and Address.prototype.toJSON (below). The only
// built-ins it reaches are node:fs and node:path, for directoryResolver,
// through process.getBuiltinModule when the resolver is made: a runtime
// without them (a browser) refuses it there, with TA_ERROR_UNSUPPORTED.
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
import { Address, compileCharts, Host, ResolveError, TinyactorsError } from '#direct';

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

// ---- shapes a JavaScript programmer would otherwise write (p3-shapes) ----

/** Defines `value` on `target` as a class's method is: writable,
 * configurable, not enumerable, its `name` the property's. */
function method(target, name, value) {
  Object.defineProperty(value, 'name', { value: name });
  Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
}

/** The options of addCharts: the compile call's resolver, nothing else. */
const ADD_CHARTS_OPTIONS = new Set(['resolver']);

// host.addCharts({name: text}, {resolver}?) → segment: compileCharts of the
// mapping's entries in their order (the first the boot chart), addImage of
// the image, close of the result, in one call. The result is closed on every
// path, so a refused image leaves nothing behind; a CompileError is
// compileCharts's own, with every diagnostic.
method(Host.prototype, 'addCharts', function (charts, options) {
  if (typeof charts !== 'object' || charts === null || Array.isArray(charts))
    throw new TypeError('addCharts: charts must be an object of document names to texts');
  if (options !== undefined) {
    if (typeof options !== 'object' || options === null) throw new TypeError('addCharts: options must be an object');
    for (const key of Object.keys(options))
      if (!ADD_CHARTS_OPTIONS.has(key)) throw new TypeError(`addCharts: unknown option ${key}`);
  }
  const sources = Object.entries(charts).map(([name, text]) => ({ name, text }));
  const compiled = compileCharts(sources, options?.resolver);
  try {
    return this.addImage(compiled.image);
  } finally {
    compiled.close();
  }
});

// host.spawnChart(name, options?) → session: spawn of the chart's current
// version, host.chart(name) then host.spawn(ref, options).
method(Host.prototype, 'spawnChart', function (name, options) {
  return this.spawn(this.chart(name), options);
});

// Address.prototype.toJSON: its text, so JSON.stringify writes an address
// as the string an Address is made from (new Address(text) reads it back).
method(Address.prototype, 'toJSON', function () {
  return this.text;
});

// directoryResolver(root) → resolver: a resolver for the compile calls that
// reads `source` as a file under `root`, relative to the directory of
// `base` (a document named `charts/order.scxml` asking for `child.scxml`
// reads `root/charts/child.scxml`), and refuses, with a ResolveError, a
// path that leaves `root`: `..`, an absolute path, or a symbolic link
// anywhere on the way that points outside. `root` is resolved through its
// own links once, when the resolver is made (a root that does not exist
// throws there). A file that does not exist is a ResolveError too, so the
// compile call reports it as a diagnostic; any other error of the file
// system (a directory, no permission) is thrown from the compile call as it
// is. The text must be UTF-8: bytes that are not are refused, never
// replaced.
const UTF8 = new TextDecoder('utf-8', { fatal: true });

/** node:fs and node:path, from the runtime (node, bun); a runtime without
 * them refuses directoryResolver. */
function builtins() {
  const get = globalThis.process?.getBuiltinModule;
  if (typeof get !== 'function') {
    throw new TinyactorsError('directoryResolver reads files with node:fs, which this runtime does not have', {
      status: 'TA_ERROR_UNSUPPORTED',
    });
  }
  return { fs: get.call(globalThis.process, 'node:fs'), path: get.call(globalThis.process, 'node:path') };
}

/** Whether `file`, absolute, is `root` or under it (`path`: node:path). */
function inside(path, root, file) {
  const rest = path.relative(root, file);
  return rest !== '' && rest !== '..' && !rest.startsWith(`..${path.sep}`) && !path.isAbsolute(rest);
}

export function directoryResolver(root) {
  const { fs, path } = builtins();
  const top = fs.realpathSync(root);
  return function resolve(base, source) {
    const file = path.resolve(top, path.dirname(base), source);
    if (!inside(path, top, file)) throw new ResolveError(`${source} (from ${base}) leaves the resolver's root`);
    let real;
    try {
      real = fs.realpathSync(file);
    } catch (error) {
      if (error?.code === 'ENOENT' || error?.code === 'ENOTDIR')
        throw new ResolveError(`${source} (from ${base}): no such file under the resolver's root`);
      throw error;
    }
    if (!inside(path, top, real)) throw new ResolveError(`${source} (from ${base}) leaves the resolver's root through a symbolic link`);
    const bytes = fs.readFileSync(real);
    try {
      return UTF8.decode(bytes);
    } catch {
      throw new ResolveError(`${source} (from ${base}) is not UTF-8`);
    }
  };
}
