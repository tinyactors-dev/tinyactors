'use strict';

// The cache of SCXML documents the invoker imports (napi ADR 0023): a
// bounded LRU keyed by a hash of the bytes. Evicting a document retires its
// definition, a child still running keeps it alive, and the next invoke
// imports it again. Resolvers go once their definition is freed. Runs on
// Node and Bun.

const assert = require('node:assert/strict');
const { createSystem } = require('..');
const { nativeOf } = require('../lib/system');
const { ecmascript } = require('../tools/ecmascript-datamodel');

const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript"';

function setup(options = {}) {
  const logs = [];
  const compiled = { count: 0 };
  const system = createSystem({
    ...options,
    log: (record) => logs.push(record.hasValue ? `${record.label}=${record.value}` : record.label),
  });
  system.registerDatamodel('ecmascript', {
    compile(source, role, info) {
      compiled.count += 1;
      return ecmascript.compile(source, role, info);
    },
  });
  return { system, logs, compiled };
}

const cache = (system) => nativeOf(system).hostMetrics().childDocuments;

const metricValue = (snapshot, name, attributes = {}) =>
  snapshot.metrics
    .find((metric) => metric.name === name)
    ?.points.filter((point) => Object.entries(attributes).every(([key, value]) => point.attributes[key] === value))
    .reduce((sum, point) => sum + point.value, 0);

// A parent that invokes a child which stays running, and forwards `poke` to it.
function parentOf(system, name, kid) {
  return system.define(`<scxml ${NS} name="${name}" initial="busy">
    <state id="busy">
      <invoke id="kid" type="scxml"><content>
        <scxml ${NS} name="${kid}" initial="k0">
          <state id="k0"><transition event="poke"><log label="poked ${kid}"/></transition></state>
        </scxml>
      </content></invoke>
      <transition event="poke"><send event="poke" target="#_kid"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
}

// The inspection of the resident child whose document is named `kid`.
function childOf(system, kid) {
  return [...system.actors()].find((view) => view.definition.family === kid);
}

// The same document hits; a different one misses. The metrics say so.
async function hitsAndMisses() {
  const { system, compiled } = setup();
  const parent = parentOf(system, 'parent', 'kidA');
  system.spawn(parent);
  await system.flush();
  const afterFirst = compiled.count;
  system.spawn(parent);
  await system.flush();
  assert.equal(compiled.count, afterFirst, 'the second invoke compiles nothing');
  assert.deepEqual({ ...cache(system), bytes: 0 }, { count: 1, bytes: 0, hits: 1, misses: 1, evictions: 0, resolvers: 0 });
  assert.ok(cache(system).bytes > 0);
  const metrics = system.metrics();
  assert.equal(metricValue(metrics, 'tinyactors.invoke.documents'), 1);
  assert.equal(metricValue(metrics, 'tinyactors.invoke.documents.size'), cache(system).bytes);
  assert.equal(metricValue(metrics, 'tinyactors.invoke.document.lookups', { 'tinyactors.lookup.result': 'hit' }), 1);
  assert.equal(metricValue(metrics, 'tinyactors.invoke.document.lookups', { 'tinyactors.lookup.result': 'miss' }), 1);
  assert.equal(metricValue(metrics, 'tinyactors.invoke.document.evictions'), 0);
  system.close();
}

// Past maxCount the least recently used document is evicted and its
// definition retired; its running child keeps working, and the next invoke
// of that document imports it again.
async function evictionKeepsRunningChildren() {
  const { system, logs } = setup({ childDocuments: { maxCount: 1 } });
  const first = system.spawn(parentOf(system, 'first', 'kidA'));
  await system.flush();
  const kidA = childOf(system, 'kidA').definition;
  system.spawn(parentOf(system, 'second', 'kidB'));
  await system.flush();
  assert.equal(cache(system).evictions, 1);
  assert.equal(cache(system).count, 1);
  assert.equal(system.findDefinition(kidA), undefined, 'an evicted definition is retired');
  system.post(first, 'poke');
  await system.flush();
  assert.deepEqual(logs, ['poked kidA'], 'the running child survives the eviction');
  system.spawn(parentOf(system, 'third', 'kidA'));
  await system.flush();
  assert.ok(system.findDefinition(kidA), 'the evicted document is imported again, under a new definition');
  assert.deepEqual({ ...cache(system), bytes: 0 }, { count: 1, bytes: 0, hits: 0, misses: 3, evictions: 2, resolvers: 0 });
  system.close();
}

// A <content expr> that builds a new document on every invoke stays within
// the bound, and shows as misses, evictions and a WARN log.
async function changingDocumentsStayBounded() {
  const { system } = setup({ childDocuments: { maxCount: 3 } });
  const logBatches = [];
  system.setTraceSink((traces, logs) => { if (logs !== undefined) logBatches.push(Buffer.from(logs)); });
  const churner = system.define(`<scxml ${NS} name="churner" initial="idle">
    <datamodel><data id="n" expr="0"/></datamodel>
    <state id="idle"><transition event="go" target="busy"><assign location="n" expr="n + 1"/></transition></state>
    <state id="busy">
      <invoke type="scxml"><content expr="\`&lt;scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript' name='kid\${n}' initial='k0'>&lt;state id='k0'/>&lt;/scxml>\`"/></invoke>
      <transition event="back" target="idle"/>
    </state>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(churner);
  await system.flush();
  for (let round = 0; round < 10; round += 1) {
    system.post(actor, 'go');
    system.post(actor, 'back');
    await system.flush();
  }
  const numbers = cache(system);
  assert.equal(numbers.count, 3, 'the cache stays at its bound');
  assert.equal(numbers.misses, 10);
  assert.equal(numbers.evictions, 7);
  assert.equal(numbers.hits, 0);
  const text = Buffer.concat(logBatches).toString('latin1');
  assert.ok(text.includes('tinyactors.invoke.document.evicted'), 'an eviction writes a WARN log');
  assert.ok(text.includes('churner'), 'the log names the invoking parent');
  system.close();
}

// A cached child definition a user retired is not used again: a miss.
async function retiredByTheUser() {
  const { system } = setup();
  const parent = parentOf(system, 'parent', 'kidA');
  system.spawn(parent);
  await system.flush();
  system.findDefinition(childOf(system, 'kidA').definition).retire();
  system.spawn(parent);
  await system.flush();
  assert.equal(cache(system).hits, 0);
  assert.equal(cache(system).misses, 2);
  assert.ok(system.findDefinition({ family: 'kidA', revision: childOf(system, 'kidA').definition.revision }),
            'the document was imported again, under a new definition');
  system.close();
}

// A resolve(src) is released once its definition is freed, not at close().
async function resolversGoWithTheirDefinitions() {
  const { system, logs } = setup();
  const files = { 'file:kid.scxml': `<scxml ${NS} name="kid" initial="k0"><state id="k0"><onentry><log label="kid"/></onentry></state></scxml>` };
  const resolve = (src) => files[src];
  const fetcher = (name) => `<scxml ${NS} name="${name}" initial="s0"><state id="s0"><invoke type="scxml" src="file:kid.scxml"/></state></scxml>`;
  const kept = system.define(fetcher('kept'), { format: 'scxml', resolve });
  for (let at = 0; at < 20; at += 1) {
    system.define(fetcher(`gone${at}`), { format: 'scxml', resolve }).retire();
  }
  assert.ok(cache(system).resolvers <= 6, `freed definitions' resolvers are swept (${cache(system).resolvers} kept)`);
  system.spawn(kept);
  await system.flush();
  assert.deepEqual(logs, ['kid'], 'a live definition keeps its resolver');
  system.close();
}

(async () => {
  await hitsAndMisses();
  await evictionKeepsRunningChildren();
  await changingDocumentsStayBounded();
  await retiredByTheUser();
  await resolversGoWithTheirDefinitions();
  console.log('documents.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
