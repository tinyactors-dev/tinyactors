'use strict';

// The in-system SCXML invoker imports the document it invokes (ADR 0021):
// a literal <content> or a src its parent's resolve(src) fetches, with the
// system's data models, cached by document bytes. Core routes #_parent,
// #_<invokeid>, done.invoke, <finalize> and autoforwarding. Runs on Node
// and Bun.

const assert = require('node:assert/strict');
const { createSystem } = require('..');
const { ecmascript } = require('../tools/ecmascript-datamodel');

const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript"';

// A system with the ECMAScript example compiler, recording <log> labels
// (with their values) and faults, and counting compiled expressions.
function setup(options = {}) {
  const logs = [];
  const faults = [];
  const compiled = { count: 0 };
  const system = createSystem({
    ...options,
    log: (record) => logs.push(record.hasValue ? `${record.label}=${record.value}` : record.label),
    fault: (record) => faults.push(record),
  });
  system.registerDatamodel('ecmascript', {
    compile(source, role, info) {
      compiled.count += 1;
      return ecmascript.compile(source, role, info);
    },
  });
  return { system, logs, faults, compiled };
}

function resident(system) {
  return [...system.actors()].length;
}

// Parent and child exchange events both ways, and done.invoke ends it.
async function exchange() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="parent" initial="s0">
    <state id="s0">
      <invoke id="kid" type="scxml"><content>
        <scxml ${NS} name="kid" initial="k0">
          <state id="k0">
            <onentry><send event="ping" target="#_parent"/></onentry>
            <transition event="pong" target="kdone"><log label="kid got pong"/></transition>
          </state>
          <final id="kdone"/>
        </scxml>
      </content></invoke>
      <transition event="ping"><log label="parent got ping"/><send event="pong" target="#_kid"/></transition>
      <transition event="done.invoke.kid" target="end"><log label="done"/></transition>
    </state>
    <final id="end"/>
  </scxml>`, { format: 'scxml' });
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['parent got ping', 'kid got pong', 'done']);
  assert.equal(resident(system), 0, 'parent and child both finished');
  system.close();
}

// Leaving the invoking state cancels the child: it is destroyed.
async function cancelOnExit() {
  const { system } = setup();
  const parent = system.define(`<scxml ${NS} name="leaver" initial="s0">
    <state id="s0">
      <invoke type="scxml"><content><scxml ${NS} name="idler"><state id="idle"/></scxml></content></invoke>
      <transition event="leave" target="s1"/>
    </state>
    <state id="s1"/>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(parent);
  await system.flush();
  assert.equal(resident(system), 2, 'the child runs');
  system.post(actor, 'leave');
  await system.flush();
  assert.equal(resident(system), 1, 'the child is gone once the invoking state exits');
  system.close();
}

// <donedata> arrives as done.invoke's data; <finalize> runs on the child's
// events before the parent's transitions see them.
async function doneDataAndFinalize() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="collector" initial="s0">
    <datamodel><data id="got" expr="0"/></datamodel>
    <state id="s0">
      <invoke id="worker" type="scxml">
        <content>
          <scxml ${NS} name="worker" initial="w0">
            <state id="w0">
              <onentry><send event="result" target="#_parent"><param name="v" expr="7"/></send></onentry>
              <transition event="finish" target="wdone"/>
            </state>
            <final id="wdone"><donedata><param name="x" expr="42"/></donedata></final>
          </scxml>
        </content>
        <finalize><assign location="got" expr="_event.data.v"/></finalize>
      </invoke>
      <transition event="result"><log label="finalized" expr="got"/><send event="finish" target="#_worker"/></transition>
      <transition event="done.invoke.worker" target="end"><log label="done" expr="_event.data.x"/></transition>
    </state>
    <final id="end"/>
  </scxml>`, { format: 'scxml' });
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['finalized=7', 'done=42']);
  system.close();
}

// autoforward="true": the parent's external events reach the child too.
async function autoforward() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="forwarder" initial="s0">
    <state id="s0">
      <invoke type="scxml" autoforward="true"><content>
        <scxml ${NS} name="listener">
          <state id="l0">
            <transition event="hello"><send event="heard" target="#_parent"/></transition>
          </state>
        </scxml>
      </content></invoke>
      <transition event="heard"><log label="child heard it"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(parent);
  await system.flush();
  system.post(actor, 'hello');
  await system.flush();
  assert.deepEqual(logs, ['child heard it']);
  system.close();
}

// A child invokes a grandchild, and an event climbs both #_parent links.
async function nested() {
  const { system, logs } = setup();
  const root = system.define(`<scxml ${NS} name="root" initial="r0">
    <state id="r0">
      <invoke type="scxml"><content>
        <scxml ${NS} name="middle" initial="m0">
          <state id="m0">
            <invoke type="scxml"><content>
              <scxml ${NS} name="leaf" initial="f0">
                <state id="f0"><onentry><send event="up" target="#_parent"/></onentry></state>
              </scxml>
            </content></invoke>
            <transition event="up"><send event="up2" target="#_parent"/></transition>
          </state>
        </scxml>
      </content></invoke>
      <transition event="up2"><log label="root heard the grandchild"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(root);
  await system.flush();
  assert.deepEqual(logs, ['root heard the grandchild']);
  assert.equal(resident(system), 3);
  system.destroy(actor);
  await system.flush();
  assert.equal(resident(system), 0, 'destroying the root destroys its child and grandchild');
  system.close();
}

// A child that faults (a runaway, core ADR 0075) is quarantined like any
// actor; the parent carries on and is not told.
async function childFaults() {
  const { system, faults } = setup({ scheduling: { microstepLimit: 50 } });
  const parent = system.define(`<scxml ${NS} name="steady" initial="s0">
    <datamodel><data id="n" expr="0"/></datamodel>
    <state id="s0">
      <invoke type="scxml"><content>
        <scxml ${NS} name="spinner" initial="spin">
          <datamodel><data id="i" expr="0"/></datamodel>
          <state id="spin"><transition target="spin"><assign location="i" expr="i + 1"/></transition></state>
        </scxml>
      </content></invoke>
      <transition event="tick"><assign location="n" expr="n + 1"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(parent);
  await system.flush();
  assert.equal(faults.length, 1);
  assert.equal(faults[0].code, 'runaway');
  system.post(actor, 'tick');
  await system.flush();
  assert.equal(system.inspect(actor).data.n, 1, 'the parent still runs');
  system.close();
}

// src is fetched with the parent definition's resolve(src), which the child
// inherits for its own src; the same document is imported once.
async function srcAndCache() {
  const { system, logs, compiled } = setup();
  const fetched = [];
  const files = {
    'file:child.scxml': `<scxml ${NS} name="child" initial="c0">
      <state id="c0">
        <invoke type="scxml" src="file:grandchild.scxml"/>
        <transition event="leaf"><send event="child" target="#_parent"/></transition>
      </state>
    </scxml>`,
    'file:grandchild.scxml': `<scxml ${NS} name="grandchild" initial="g0">
      <state id="g0"><onentry><send event="leaf" target="#_parent"/></onentry></state>
    </scxml>`,
  };
  const resolve = (src) => {
    fetched.push(src);
    return files[src];
  };
  const parent = system.define(`<scxml ${NS} name="fetcher" initial="a">
    <state id="a">
      <invoke type="scxml" src="file:child.scxml"/>
      <transition event="child" target="b"><log label="first"/></transition>
    </state>
    <state id="b">
      <invoke type="scxml" src="file:child.scxml"/>
      <transition event="child" target="end"><log label="second"/></transition>
    </state>
    <final id="end"/>
  </scxml>`, { format: 'scxml', resolve });
  const before = compiled.count;
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['first', 'second']);
  assert.deepEqual(fetched, ['file:child.scxml', 'file:grandchild.scxml', 'file:child.scxml', 'file:grandchild.scxml']);
  const afterFirstRun = compiled.count - before;
  // A second parent invokes the same documents: nothing is compiled again.
  system.spawn(parent);
  await system.flush();
  assert.equal(compiled.count - before, afterFirstRun, 'the cached child definitions are reused');
  system.close();
}

// Only <param> and namelist names the child declares enter its data (SCXML 6.4).
async function params() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="giver" initial="s0">
    <datamodel><data id="a" expr="1"/><data id="b" expr="2"/></datamodel>
    <state id="s0">
      <invoke type="scxml" namelist="a b"><content>
        <scxml ${NS} name="taker" initial="t0">
          <datamodel><data id="a" expr="0"/></datamodel>
          <state id="t0"><onentry>
            <log label="a" expr="a"/>
            <log label="b declared" expr="typeof b !== 'undefined'"/>
          </onentry></state>
        </scxml>
      </content></invoke>
    </state>
  </scxml>`, { format: 'scxml' });
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['a=1', 'b declared=false']);
  system.close();
}

// A child document that cannot be imported, or a src with no resolver,
// raises error.execution in the parent (SCXML 6.4).
async function refusals() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="refuser" initial="s0">
    <state id="s0">
      <invoke type="scxml"><content><scxml ${NS} name="broken"><state id="x"><transition target="nowhere"/></state></scxml></content></invoke>
      <transition event="error.execution" target="s1"><log label="refused"/></transition>
    </state>
    <state id="s1">
      <invoke type="scxml" src="file:missing.scxml"/>
      <transition event="error.execution" target="s2"><log label="no resolver"/></transition>
    </state>
    <state id="s2"/>
  </scxml>`, { format: 'scxml' });
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['refused', 'no resolver']);
  system.close();
}

(async () => {
  await exchange();
  await cancelOnExit();
  await doneDataAndFinalize();
  await autoforward();
  await nested();
  await childFaults();
  await srcAndCache();
  await params();
  await refusals();
  console.log('invoke.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
