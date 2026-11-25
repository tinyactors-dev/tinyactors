// The binding's own blocks of a host: its tables, its arenas and the
// native datamodel's kept writer, counted by the allocator the addon gives
// them (src/module.c, ta_napi_counted_allocator), apart from core's host,
// which counts its own (host.stats' allocatedBytes). The addon's
// bindingMemory(host) reads the count; lib/direct.mjs does not export it,
// so no application sees it (docs/proposals/memory-accounting.md, O1):
// this test loads the addon itself.
import { createRequire } from 'node:module';
import { DATAMODEL, equal, hostWith, scxml, throws, typeError } from './charts.mjs';
import { run, test } from './harness.mjs';

const { bindingMemory } = createRequire(import.meta.url)('../build/tinyactors.node');

const COUNTER = scxml('counter', `
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="s"><transition event="inc"><assign location="count" expr="count + 1"/></transition></state>`);

test('the binding\'s own blocks are counted, and none is left after close', async () => {
  const host = hostWith({ counter: COUNTER });
  const start = bindingMemory(host);
  const actors = [];
  for (let i = 0; i < 1000; i++) actors.push(host.spawn('counter'));
  for (const actor of actors) actor.send('inc');
  await host.idle();
  const grown = bindingMemory(host);
  // a native session is a row of 24 B in the binding's session table
  if (DATAMODEL === 'native' && !(grown.bytes >= start.bytes + 1000 * 24))
    throw new Error(`1000 native sessions: the binding's bytes went from ${start.bytes} to ${grown.bytes}`);
  if (!(grown.bytes > 0 && grown.blocks > 0 && grown.peak >= grown.bytes))
    throw new Error(`an open host's binding memory: ${JSON.stringify(grown)}`);
  host.close();
  const closed = bindingMemory(host);
  equal(closed.bytes, 0, 'the binding\'s bytes after close');
  equal(closed.blocks, grown.blocks, 'close allocates nothing');
});

test('bindingMemory refuses what is not a Host', () => {
  throws(() => bindingMemory({}), typeError, 'a plain object');
  throws(() => bindingMemory(), typeError, 'nothing');
});

await run();
