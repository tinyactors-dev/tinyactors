import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';

// wasm memory is the binding's to give back: a system's blocks on destroy, an
// image's when nothing holds it (a FinalizationRegistry), and a system holds
// the images it runs. A separate process gives deterministic GC pressure
// without changing the test runner's flags.
test('wasm memory: systems give theirs back on destroy, images live while a system runs them', () => {
  const child = spawnSync(process.execPath, ['--expose-gc', '--input-type=module', '-e', `
    import assert from 'node:assert/strict';
    import { readFileSync } from 'node:fs';
    import { createNative } from ${JSON.stringify(new URL('../native.mjs', import.meta.url).href)};
    const instance = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(new URL(${JSON.stringify(new URL('../tinyactors.wasm', import.meta.url).href)}))), {});
    const native = createNative(instance);
    const xml = '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="idle">' +
      '<datamodel><data id="n" expr="0"/></datamodel><state id="idle"><transition event="inc"><assign location="n" expr="n + 1"/></transition></state></scxml>';
    const turn = () => new Promise((resolve) => setTimeout(resolve, 2));
    const callbacks = (owner) => ({ resolve: (source, texts) => texts.map((t) => (t === '0' ? () => 0 : t === 'n + 1' ? (s) => s.n + 1 : t === 'n' ? (s, v) => { s.n = v; } : undefined)), effects: () => owner, sync() {}, ref: (a) => a });
    const round = (ecmascript) => {
      const owner = {};
      owner.system = native.createSystem(callbacks(owner), { systemID: 'gc', ecmascript });
      native.addImage(owner.system, native.compile([{ name: 'gc', xml }], []));
      const session = native.spawn(owner.system, 0, 0, undefined, undefined, undefined, 0);
      native.pump(owner.system, 0);
      for (let i = 0; i < 20; i++) native.send(owner.system, session, 'inc', undefined, undefined, 0);
      native.destroy(owner.system);
    };
    /* a held system keeps its image through collections */
    const owner = {};
    owner.system = native.createSystem(callbacks(owner), { systemID: 'held' });
    native.addImage(owner.system, native.compile([{ name: 'gc', xml }], []));
    for (let i = 0; i < 5; i++) { global.gc(); await turn(); }
    const session = native.spawn(owner.system, 0, 0, undefined, undefined, undefined, 0);
    native.pump(owner.system, 0);
    assert.ok(session > 0, 'the image a system runs survives collections');
    native.destroy(owner.system);
    /* many systems, both datamodels: memory steady */
    const pages = [];
    for (let i = 0; i < 300; i++) {
      round(i % 2 ? 'quickjs' : 'native');
      if (i % 50 === 49) { for (let k = 0; k < 3; k++) { global.gc(); await turn(); } pages.push(instance.exports.memory.buffer.byteLength / 65536); }
    }
    assert.equal(pages.at(-1), pages[1], 'memory grows with every system: ' + pages.join(' '));
  `], { encoding: 'utf8', timeout: 60000 });
  assert.equal(child.status, 0, child.stderr || String(child.error));
});
