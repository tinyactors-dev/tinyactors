'use strict';

// An invoked child that runs away (core ADR 0075) is abandoned by core
// (core ADR 0099 section 7), not quarantined: the fault hook hears of it, the
// child keeps the configuration of its last microstep and stands stable, and
// its parent is not told (napi ADR 0025's `.quarantined` event goes with the
// quarantine, core ADR 0103 section 8): the invocation stays active, and a
// child that finishes later still sends done.invoke. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem } = require('..');
const { ecmascript } = require('../tools/ecmascript-datamodel');

const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript"';

// Runs away (80 microsteps against a limit of 50) when it starts, and again
// on the second `again`; between them it finishes its macrostep, and the
// third `again` ends in a final state.
const TWICE = `<scxml ${NS} name="twice" initial="spin1">
  <datamodel><data id="i" expr="0"/></datamodel>
  <state id="spin1">
    <transition cond="i &lt; 80" target="spin1"><assign location="i" expr="i + 1"/></transition>
    <transition cond="i &gt;= 80" target="idle"/>
  </state>
  <state id="idle">
    <transition event="again" target="spin2"><assign location="i" expr="0"/></transition>
  </state>
  <state id="spin2">
    <transition cond="i &lt; 80" target="spin2"><assign location="i" expr="i + 1"/></transition>
    <transition cond="i &gt;= 80" target="done"/>
  </state>
  <final id="done"/>
</scxml>`;

// Waits in `waiting` for its child `w`, keeping every error report.
function waiter(child) {
  return `<scxml ${NS} name="waiter" initial="waiting">
    <datamodel><data id="errors" expr="[]"/></datamodel>
    <state id="waiting">
      <invoke type="scxml" id="w"><content>${child}</content></invoke>
      <transition event="error.communication.invoke.w.quarantined">
        <assign location="errors" expr="errors.concat([_event.name])"/>
      </transition>
      <transition event="poke"><send event="again" target="#_w"/></transition>
      <transition event="done.invoke.w" target="finished"/>
      <transition event="leave" target="left"/>
    </state>
    <state id="finished"/>
    <state id="left"/>
  </scxml>`;
}

function setup(options = {}) {
  const faults = [];
  const system = createSystem({
    parameters: { microstepLimit: 50 },
    ...options,
    fault: (record) => {
      faults.push(record);
      if (options.onFault) options.onFault(record);
    },
  });
  system.registerDatamodel('ecmascript', ecmascript);
  return { system, faults };
}

// The fault hook hears of the runaway; the child is stable, and the parent
// keeps waiting without being told.
async function notTold() {
  const { system, faults } = setup();
  const parent = system.spawn(system.define(waiter(TWICE), { format: 'scxml' }));
  await system.flush();
  assert.equal(faults.length, 1);
  assert.equal(faults[0].code, 'runaway');
  const view = parent.inspect();
  assert.deepEqual(view.data.errors, [], 'the parent is not told');
  assert.deepEqual(view.activeStates, ['waiting']);
  assert.equal(faults[0].actor.inspect().scheduling, 'idle', 'the child is stable, not quarantined');
  assert.deepEqual(faults[0].actor.inspect().activeStates, ['spin1']);
  system.close();
}

// The child carries on with the next input: a second runaway reports again,
// and a child that finishes still sends done.invoke.
async function childFinishes() {
  const { system, faults } = setup();
  const parent = system.spawn(system.define(waiter(TWICE), { format: 'scxml' }));
  await system.flush();
  const child = faults[0].actor;

  parent.post('poke'); // `again` enables nothing in spin1, which finishes its loop
  await system.flush();
  assert.deepEqual(child.inspect().activeStates, ['idle']);
  assert.equal(faults.length, 1);

  parent.post('poke');
  await system.flush();
  assert.equal(faults.length, 2, 'the child ran away again');
  assert.deepEqual(child.inspect().activeStates, ['spin2']);

  parent.post('poke');
  await system.flush();
  assert.deepEqual(parent.inspect().activeStates, ['finished'], 'done.invoke arrived');
  assert.deepEqual(parent.inspect().data.errors, []);
  system.close();
}

// A parent that leaves the invoking state cancels, and destroys, the child.
async function parentLeaves() {
  const { system, faults } = setup();
  const parent = system.spawn(system.define(waiter(TWICE), { format: 'scxml' }));
  await system.flush();
  const child = faults[0].actor;
  parent.post('leave');
  await system.flush();
  assert.deepEqual(parent.inspect().activeStates, ['left']);
  assert.equal(child.destroyed, true, 'the child is destroyed with its invocation');
  assert.equal([...system.actors()].length, 1);
  system.close();
}

// A parent destroyed from the fault hook: nothing crashes or waits.
async function parentDestroyed() {
  let parent = null;
  const { system, faults } = setup({ onFault: () => system.destroy(parent) });
  parent = system.spawn(system.define(waiter(TWICE), { format: 'scxml' }));
  await system.flush();
  assert.equal(faults.length, 1);
  assert.equal(parent.destroyed, true);
  assert.equal([...system.actors()].length, 0, 'the child went with its parent');
  system.close();
}

(async () => {
  await notTold();
  await childFinishes();
  await parentLeaves();
  await parentDestroyed();
  console.log('quarantine.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
