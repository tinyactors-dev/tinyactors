'use strict';

// The in-system SCXML invoker starts child charts (napi ADRs 0021 and
// 0024, core ADR 0077). An inline <scxml> in <content>, or a literal src
// the parent's resolve(src) fetches, is compiled when the parent is defined
// and nested in it; chart text from <content expr> is compiled at invoke
// time, reused by identity while one is alive, and freed with its last
// child. Core routes #_parent, #_<invokeid>, done.invoke, <finalize> and
// autoforwarding. Runs on Node and Bun.

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
async function srcNested() {
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
  // Fetched while the parent is defined, a chart's own <invoke>s first: the
  // second <invoke> of the same src answers the child already compiled, and
  // its grandchild is compiled once.
  assert.deepEqual(fetched, ['file:child.scxml', 'file:child.scxml', 'file:grandchild.scxml']);
  const before = compiled.count;
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['first', 'second']);
  system.spawn(parent);
  await system.flush();
  assert.equal(compiled.count, before, 'invokes compile nothing');
  assert.equal(fetched.length, 3, 'invokes fetch nothing');
  system.close();
}

// An inline child is compiled once, with its parent; a broken one refuses
// the parent at define time, saying which <invoke> and where.
async function inlineAtDefine() {
  const { system, logs, compiled } = setup();
  const text = `<scxml ${NS} name="holder" initial="s0">
    <state id="s0">
      <invoke type="scxml"><content>
        <scxml ${NS} initial="c0"><state id="c0"><onentry><log label="child"/></onentry></state></scxml>
      </content></invoke>
    </state>
  </scxml>`;
  const parent = system.define(text, { format: 'scxml' });
  const afterDefine = compiled.count;
  system.spawn(parent);
  system.spawn(parent);
  await system.flush();
  assert.deepEqual(logs, ['child', 'child']);
  assert.equal(compiled.count, afterDefine, 'the nested child was compiled at define time');
  // Defining the same chart again answers the same definition.
  assert.equal(system.define(text, { format: 'scxml' }).id, parent.id);
  assert.throws(
    () => system.define(`<scxml ${NS} name="refuser" initial="s0"><state id="s0">
      <invoke type="scxml"><content><scxml ${NS} name="broken"><state id="x"><transition target="nowhere"/></state></scxml></content></invoke>
    </state></scxml>`, { format: 'scxml' }),
    (error) => /child chart of <invoke> 0 is refused at line 1/.test(error.message));
  system.close();
}

const MAKER = `<script><![CDATA[
  function make(n) {
    return '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="c0">' +
      '<datamodel><data id="n" expr="' + n + '"/></datamodel>' +
      '<state id="c0"><transition event="stop" target="done"/></state><final id="done"/></scxml>';
  }
]]></script>`;

// <content expr> text is compiled at invoke time, reused by identity while
// a child of it is alive, and freed with its last child.
async function contentExpr() {
  const { system, compiled } = setup();
  const parent = system.define(`<scxml ${NS} name="maker" initial="s0">
    ${MAKER}
    <state id="s0">
      <invoke type="scxml" id="kid"><content expr="make(1)"/></invoke>
      <transition event="stop"><send event="stop" target="#_kid"/></transition>
      <transition event="done.invoke.kid" target="idle"/>
    </state>
    <state id="idle"/>
  </scxml>`, { format: 'scxml' });
  const first = system.spawn(parent);
  await system.flush();
  const second = system.spawn(parent);
  await system.flush();
  const children = [...system.actors()].map((view) => view.definition)
    .filter((identity) => identity.family === 'maker/invoke');
  assert.equal(children.length, 2, 'two children, of an unnamed chart in the family "maker/invoke"');
  assert.deepEqual(children[0], children[1], 'the second invoke was answered by the live definition');
  assert.notEqual(system.findDefinition(children[0]), undefined);
  system.post(first, 'stop');
  system.post(second, 'stop');
  await system.flush();
  assert.equal(system.findDefinition(children[0]), undefined, 'freed with its last child');
  assert.ok(compiled.count > 0);
  system.close();
}

// Text that differs on every invoke does not grow the system: each child
// definition goes with its child.
async function varyingText() {
  const { system } = setup();
  const parent = system.define(`<scxml ${NS} name="churn" initial="s0">
    ${MAKER}
    <datamodel><data id="i" expr="0"/></datamodel>
    <state id="s0">
      <invoke type="scxml" id="kid"><content expr="make(i)"/></invoke>
      <transition event="done.invoke.kid" target="s0"><assign location="i" expr="i + 1"/></transition>
      <transition event="next"><send event="stop" target="#_kid"/></transition>
    </state>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(parent);
  await system.flush();
  const child = () => [...system.actors()].map((view) => view.definition)
    .find((identity) => identity.family === 'churn/invoke');
  const seen = [];
  for (let round = 0; round < 50; round += 1) {
    seen.push(child());
    system.post(actor, 'next');
    await system.flush();
  }
  assert.equal(actor.inspect().data.i, 50);
  assert.equal(new Set(seen.map((identity) => identity.revision)).size, 50, '50 distinct child charts');
  assert.deepEqual(seen.filter((identity) => system.findDefinition(identity) !== undefined), [],
                   'each child chart went with its child');
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

// Chart text at invoke time that cannot be compiled, or a src with no
// resolver, raises error.execution in the parent (SCXML 6.4).
async function refusals() {
  const { system, logs } = setup();
  const parent = system.define(`<scxml ${NS} name="refuser" initial="s0">
    ${MAKER}
    <state id="s0">
      <invoke type="scxml"><content expr="'&lt;scxml'"/></invoke>
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
  await srcNested();
  await inlineAtDefine();
  await contentExpr();
  await varyingText();
  await params();
  await refusals();
  console.log('invoke.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
