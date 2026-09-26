'use strict';

// system.definitions() streams every definition core holds (core ADR 0078):
// builder and imported charts, nested inline children and invoke-time
// children. metrics() and watchTraces({ families }) see the same set, so a
// child chart core registers on its own is reported and watchable. Runs on
// Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, decodeTraces } = require('..');
const { ecmascript } = require('../tools/ecmascript-datamodel');

const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript"';

const MAKER = `<script><![CDATA[
  function make(n) {
    return '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="c0">' +
      '<datamodel><data id="n" expr="' + n + '"/></datamodel>' +
      '<state id="c0"><transition event="stop" target="done"/></state><final id="done"/></scxml>';
  }
]]></script>`;

function setup(options = {}) {
  const system = createSystem(options);
  system.registerDatamodel('ecmascript', { compile: (source, role, info) => ecmascript.compile(source, role, info) });
  return system;
}

// A parent whose <invoke> nests an inline child named "kid".
function nesting(system) {
  return system.define(`<scxml ${NS} name="nester" initial="s0">
    <state id="s0">
      <invoke id="kid" type="scxml"><content>
        <scxml ${NS} name="kid" initial="k0">
          <state id="k0"><transition event="stop" target="kdone"/></state>
          <final id="kdone"/>
        </scxml>
      </content></invoke>
      <transition event="stop"><send event="stop" target="#_kid"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
}

// A parent whose <invoke> compiles its child from <content expr> text.
function making(system) {
  return system.define(`<scxml ${NS} name="maker" initial="s0">
    ${MAKER}
    <state id="s0">
      <invoke type="scxml" id="kid"><content expr="make(1)"/></invoke>
      <transition event="stop"><send event="stop" target="#_kid"/></transition>
      <transition event="done.invoke.kid" target="idle"/>
    </state>
    <state id="idle"/>
  </scxml>`, { format: 'scxml' });
}

const families = (system, options) => [...system.definitions(options)].map((view) => view.identity.family).sort();

// Every kind of definition is streamed, in pages as small as one.
async function streams() {
  const system = setup();
  system.define(statechart({ family: 'built', revision: 'v1' }).state('a'));
  const nester = nesting(system);
  const maker = making(system);
  assert.deepEqual(families(system), ['built', 'kid', 'maker', 'nester']);

  const kid = [...system.definitions()].find((view) => view.identity.family === 'kid');
  const parent = nester.inspect();
  assert.deepEqual(parent.nested, [kid.id], 'the parent names its nested child');
  assert.equal(kid.nestedBy, 1);
  assert.equal(kid.holds, 0, 'nothing but its parent holds it');
  assert.equal(parent.holds, 1);

  system.spawn(maker);
  await system.flush();
  assert.deepEqual(families(system, { pageSize: 1 }), ['built', 'kid', 'maker', 'maker/invoke', 'nester'],
                   'the invoke-time child is listed, and pages of one miss nothing');
  assert.throws(() => system.definitions({ pageSize: 0 }).next(), { code: 'invalid' });
  system.close();
}

// Freed definitions drop out; retired ones in use stay until their last actor.
async function freed() {
  const system = setup();
  const built = system.define(statechart({ family: 'built', revision: 'v1' }).state('a'));
  const actor = system.spawn(built);
  await system.flush();
  built.retire();
  const retired = [...system.definitions()].find((view) => view.identity.family === 'built');
  assert.equal(retired?.retired, true, 'retired but still used: listed');
  system.destroy(actor);
  await system.flush();
  assert.deepEqual(families(system), [], 'freed with its last actor');

  const maker = making(system);
  const parent = system.spawn(maker);
  await system.flush();
  assert.ok(families(system).includes('maker/invoke'));
  system.post(parent, 'stop');
  await system.flush();
  assert.deepEqual(families(system), ['maker'], 'the invoke-time child went with its child');
  system.close();
}

// metrics() reports a nested child's counters and occupancy under its family.
async function metrics() {
  const system = setup();
  const nester = nesting(system);
  system.spawn(nester);
  system.spawn(nester);
  await system.flush();
  const snapshot = system.metrics();
  const occupancy = snapshot.metrics.find((metric) => metric.name === 'tinyactors.actors.occupancy');
  const kid = occupancy.points.find((point) => point.attributes['tinyactors.definition.family'] === 'kid' &&
                                               point.attributes['scxml.state'] === 'k0');
  assert.equal(kid?.value, 2, 'two nested children in k0');
  const macrosteps = snapshot.metrics.find((metric) => metric.name === 'tinyactors.macrosteps');
  assert.ok(macrosteps.points.some((point) => point.attributes['tinyactors.definition.family'] === 'kid' &&
                                                point.value > 0), 'the nested child counts its macrosteps');
  system.close();
}

// watchTraces({ families }) catches invoke-time children, registered by core
// after the watch began.
async function watch() {
  const system = setup();
  const batches = [];
  system.setTraceSink((traces) => batches.push(traces), { sample: 0, logs: false });
  const handle = system.watchTraces({ families: ['maker/invoke'] });
  const maker = making(system);
  const parent = system.spawn(maker);
  await system.flush();
  const child = [...system.actors()].find((view) => view.definition.family === 'maker/invoke');
  assert.ok(child, 'the child is resident');
  batches.length = 0;
  system.post(parent, 'stop');
  await system.flush();
  const spans = batches.flatMap((bytes) => decodeTraces(bytes));
  const of = (session) => spans.filter((span) => span.attributes['scxml.session_id'] === String(session));
  assert.ok(of(child.sessionID).length > 0, 'the watched child was traced');
  // The parent's own delivery of "stop" is not watched; only what the
  // watched child causes in it (done.invoke) is, as napi ADR 0017 says.
  const parentEvents = of(system.inspect(parent).sessionID).map((span) => span.attributes['scxml.event.name'])
    .filter((name) => name !== undefined);
  assert.ok(!parentEvents.includes('stop'), `the unwatched parent's delivery was not traced (${parentEvents})`);
  handle.stop();
  system.close();
}

(async () => {
  await streams();
  await freed();
  await metrics();
  await watch();
  console.log('definitions.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
