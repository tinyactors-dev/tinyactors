// The race, off the page's thread: both engines compiled from their wasm
// modules, each program checked to give the same result in both, then timed.
// Posts { type: 'ready', startup }, then { type: 'case', ... } per program,
// then { type: 'done' }, or { type: 'error', message }.
import { QuickJS } from 'quickjs-wasi';
import quickjsURL from 'quickjs-wasi/quickjs.wasm?url';
import quickjsPackage from 'quickjs-wasi/package.json';
import { StaticJS } from './runtime.js';
import { raceCases } from './race-cases.js';
import { measure, quickjsRunner, staticjsRunner } from './race.js';

const now = () => performance.now();

async function compile(url) {
  const response = await fetch(url);
  const bytes = await response.arrayBuffer();
  return { module: await WebAssembly.compile(bytes), bytes: bytes.byteLength };
}

/** The median time to make a fresh engine from its compiled module, in ms. */
async function startup(make) {
  const times = [];
  for (let i = 0; i < 5; i++) {
    const at = now();
    const engine = await make();
    times.push(now() - at);
    engine.dispose?.();
  }
  return times.sort((a, b) => a - b)[2];
}

self.onmessage = async ({ data }) => {
  if (data.type !== 'run') return;
  try {
    const [sjsModule, qjsModule] = await Promise.all([compile(data.staticjsURL), compile(quickjsURL)]);
    const ready = {
      staticjs: { bytes: sjsModule.bytes, ms: await startup(() => new StaticJS(sjsModule.module)) },
      quickjs: { bytes: qjsModule.bytes, ms: await startup(() => QuickJS.create({ wasm: qjsModule.module })), version: quickjsPackage.version },
    };
    self.postMessage({ type: 'ready', startup: ready });
    const staticjs = new StaticJS(sjsModule.module);
    const quickjs = await QuickJS.create({ wasm: qjsModule.module });
    for (const c of raceCases) {
      if (data.only && !data.only.includes(c.id)) continue;
      self.postMessage({ type: 'running', id: c.id });
      const a = staticjsRunner(staticjs, c.source);
      const b = quickjsRunner(quickjs, c.source);
      const value = a.check();
      const same = value === b.check();
      // Timed only when both give the same result: otherwise there is
      // nothing to compare.
      const times = same ? { staticjs: measure(a, { now }), quickjs: measure(b, { now }) } : null;
      self.postMessage({ type: 'case', id: c.id, value, same, times });
    }
    quickjs.dispose();
    self.postMessage({ type: 'done' });
  } catch (e) {
    self.postMessage({ type: 'error', message: String(e?.message ?? e) });
  }
};
