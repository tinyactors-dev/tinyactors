import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';

test('wasm externals: a host retains its image, then both finalize exactly once', () => {
  // A separate process enables deterministic GC pressure without changing the
  // test runner's flags. Observe real C finalizers, not a mock registry.
  const child = spawnSync(process.execPath, ['--expose-gc', '--input-type=module', '-e', `
    import assert from 'node:assert/strict';
    import { readFileSync } from 'node:fs';
    import { createNative } from ${JSON.stringify(new URL('../native.mjs', import.meta.url).href)};
    let finalized = 0;
    const module = new WebAssembly.Module(readFileSync(new URL(${JSON.stringify(new URL('../tinyactors.wasm', import.meta.url).href)})));
    const native = createNative((imports) => {
      const instance = new WebAssembly.Instance(module, imports);
      return { exports: { ...instance.exports, w_finalize(...args) {
        finalized++;
        instance.exports.w_finalize(...args);
      } } };
    });
    function createHost() {
      // Like core.mjs, callbacks close over their owner, which owns the handle.
      // Destroy must break these strong-reference cycles before GC can run.
      const owner = {};
      owner.host = native.createHost(() => { void owner.host; return []; },
        { decode: () => owner }, 'gc', {});
      return owner.host;
    }
    let host = createHost();
    (() => {
      const image = native.compile([{ name: 'gc', xml: '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="xstate"><state id="idle"/></scxml>' }]);
      native.addImage(host, image);
      native.spawn(host, 0, 0);
      native.run(host, 10000);
    })();
    const turn = () => new Promise((resolve) => setTimeout(resolve, 5));
    for (let i = 0; i < 5; i++) { global.gc(); await turn(); }
    assert.equal(finalized, 0, 'the C image reference survives call-scope cleanup');
    native.destroy(host);
    host = null;
    for (let i = 0; i < 100 && finalized < 2; i++) { global.gc(); await turn(); }
    assert.equal(finalized, 2, 'host and image finalizers both run');
    for (let i = 0; i < 5; i++) { global.gc(); await turn(); }
    assert.equal(finalized, 2, 'no double finalization after explicit destroy');
  `], { encoding: 'utf8', timeout: 10000 });
  assert.equal(child.status, 0, child.stderr || String(child.error));
});
