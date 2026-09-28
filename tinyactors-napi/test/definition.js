'use strict';

// Step 4: defining charts (ROADMAP step 4, ADRs 0001 and 0013).

const assert = require('node:assert/strict');
const { createSystem, statechart, literal, TinyactorsError } = require('..');
const { core } = require('../lib/native');
const { nativeOf } = require('../lib/system');
const { ecmascript } = require('../tools/ecmascript-datamodel');

function assertCode(fn, code, pattern) {
  assert.throws(fn, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, code, error.message);
    if (pattern) {
      assert.match(error.message, pattern);
    }
    return true;
  });
}

// Ported from the first addon's test/definition.js, in the generated shape.
function definitions() {
  const system = createSystem();
  const definition = system.define({
    identity: { family: { name: 'worker' }, revision: 'v1' },
    initial: { targets: ['idle'] },
    states: [
      { kind: 'atomic', id: 'idle', transitions: [{ kind: 'external', event: 'work', targets: ['finished'] }] },
      { kind: 'final', id: 'finished' },
    ],
  });

  assert.equal(definition.system, system);
  assert.equal(typeof definition.id, 'bigint');
  assert.deepEqual(definition.identity, { family: 'worker', revision: 'v1' });
  assert.equal(definition.retired, false);
  assert.equal(system.findDefinition({ family: 'worker', revision: 'v1' }).id, definition.id);
  definition.retire();
  // Core frees a retired definition that no actor uses; it still reads as retired.
  assert.equal(definition.retired, true);
  definition.retire();
  assert.equal(system.findDefinition({ family: 'worker', revision: 'v1' }), undefined);

  // A revision that is not UTF-8 reads back as bytes.
  const bytes = system.define({
    identity: { family: { name: 'bytes' }, revision: new Uint8Array([0xff, 0x00, 0x01]) },
    states: [{ kind: 'atomic', id: 'only' }],
  });
  assert.deepEqual([...bytes.identity.revision], [0xff, 0x00, 0x01]);
  bytes.retire();

  // The first addon refused a document name; core accepts and reports it.
  const named = system.define({
    name: 'named',
    identity: { family: { name: 'worker' }, revision: 'v2' },
    states: [{ kind: 'atomic', id: 'idle' }],
  });
  assert.equal(named.inspect().name, 'named');
  assert.equal(named.inspect().datamodel, 'javascript');

  system.close();
  assertCode(() => definition.retired, 'closed');
  assertCode(() => named.inspect(), 'closed');
  assertCode(() => system.define(statechart({ family: 'x', revision: 'v1' }).state('a')), 'closed');
}

// The builder emits the generated shape, which round-trips through export.
function builder() {
  const system = createSystem();
  const chart = statechart({ family: 'player', revision: 'player/v1', name: 'player' })
    .initial('idle')
    .state('idle', (idle) => {
      idle.on('serve', (t) => t.target('rallying').raise('served'));
      idle.on('flip', (t) => t.when("In('idle')").target('rallying').internal());
    })
    .state('rallying', (rallying) => {
      rallying.initial('hit');
      rallying.entry((a) => a.log('rally').send('ball', (s) => s.id('ball').to('#_actor_player/pong').after('1s')));
      rallying.entry((a) => a.choose("In('hit')", (then) => then.raise('yes'), (otherwise) => otherwise.raise('no')));
      rallying.exit((a) => a.cancel('ball'));
      rallying.state('hit');
      rallying.history('last', 'deep', (h) => h.always((t) => t.target('hit')));
      rallying.on('done', (t) => t.target('over'));
    })
    .parallel('both', (both) => {
      both.state('left');
      both.state('right', (right) => right.invoke((i) => i.id('child').type('scxml').source('child.scxml')
        .autoforward().finalize((a) => a.raise('finalized'))));
    })
    .final('over');

  const definition = system.define(chart);
  assertCode(() => chart.state('late'), 'invalid', /already built/);

  const exported = core.definition_export(nativeOf(system), definition.id);
  const rallying = exported.states[1];
  assert.equal(rallying.kind, 'compound');
  assert.deepEqual(rallying.initial.targets, ['hit']);
  assert.equal(rallying.onentry.length, 2, 'each entry() is its own block');
  assert.deepEqual(rallying.onentry[0].actions[1].send.delay, { kind: 'literal', literal: '1s' });
  assert.equal(rallying.states[1].kind, 'history_deep');
  assert.equal(exported.states[0].transitions[1].kind, 'internal');
  assert.equal(exported.states[0].transitions[1].condition, "In('idle')");
  assert.equal(exported.states[2].kind, 'parallel');
  assert.equal(exported.states[2].states[1].invokes[0].autoforward, true);
  assert.equal(exported.states[3].kind, 'final');

  // Export compiles to the same chart: a plain spec in the generated shape.
  const again = system.define({ ...exported, identity: { family: { name: 'player' }, revision: 'player/v1b' } });
  assert.equal(again.inspect().stateCount, definition.inspect().stateCount);

  // Every chart compiles against the JavaScript data model (step 11); only a
  // spec that says so gets the null data model.
  const fresh = (revision) => statechart({ family: 'later', revision });
  assert.equal(definition.inspect().datamodel, 'javascript');
  assert.equal(system.define({ ...fresh('v0').state('a').build(), datamodel: 'null' }).inspect().datamodel, 'null');
  assert.equal(system.define(fresh('v1').data('count', 0).state('a')).inspect().datamodel, 'javascript');
  assert.equal(system.define(fresh('v2').state('a', (s) => s.on('e', (t) => t.when(() => true)))).inspect().datamodel,
    'javascript');
  assertCode(() => fresh('v3').state('a', (s) => s.entry((a) => a.assignAt({}, 1))), 'invalid', /get and set/);
  assertCode(() => fresh('v4').state('a', (s) => s.entry((a) => a.script('text'))), 'invalid');
  system.close();
}

// Malformed specs fail with the codec's path; refused charts name the fault.
function refusals() {
  const system = createSystem();
  assertCode(() => system.define({ identity: { family: { name: 'bad' }, revision: 'v1' }, states: [{ kind: 'atomic', id: 7 }] }),
    'invalid', /spec\.states\[0\]\.id: expected a string/);
  assertCode(() => system.define('nope'), 'invalid');
  assertCode(() => statechart({ family: 'x', revision: 1 }), 'invalid');

  // Source text is only In(state) or a data name: anything else is refused
  // when the chart is defined, naming the expression and its path. The null
  // data model, which a spec opts into, accepts only In(state).
  const expression = statechart({ family: 'expr', revision: 'v1' })
    .state('a', (a) => a.on('e', (t) => t.when('x > 1').target('a')))
    .build();
  assertCode(() => system.define(expression), 'invalid',
    /condition: expected a function, or source text: In\(state\), a data name, name\(\) or JSON "x > 1"/);
  assert.throws(() => system.define({ ...expression, datamodel: 'null' }), (error) => {
    assert.ok(error instanceof TinyactorsError);
    assert.equal(error.code, 'invalid');
    assert.equal(error.detail.code, 'expression_rejected');
    assert.equal(error.detail.path, 'states[0].transitions[0].condition');
    assert.equal(error.detail.error.subject, 'x > 1');
    assert.match(error.message, /chart refused: .*condition: the null data model has only the In\(state\) predicate "x > 1"/);
    return true;
  });

  // An unknown target names the state it could not find.
  const target = statechart({ family: 'target', revision: 'v1' }).state('a', (a) => a.on('e', (t) => t.target('nope')));
  assert.throws(() => system.define(target), (error) => {
    assert.equal(error.detail.code, 'unknown_state');
    assert.equal(error.detail.path, 'states[0].transitions[0].targets[0]');
    return true;
  });

  // In() used as a value compiles; running it faults as error.execution,
  // which step 5 checks once actors run.
  const asValue = statechart({ family: 'value', revision: 'v1' }).state('a', (a) => a.entry((b) => b.log('in', "In('a')")));
  assert.equal(system.define(asValue).inspect().stateCount, 1);
  system.close();
}

// Nothing about a definition lives in the addon: the facade holds a system
// and an ID, and every fact comes from core.
function nothingStored() {
  const system = createSystem();
  const definition = system.define(statechart({ family: 'facts', revision: 'v1' }).state('a'));
  assert.deepEqual(Object.keys(definition).sort(), ['id', 'system']);
  const view = core.definition_inspect(nativeOf(system), definition.id);
  const inspection = definition.inspect();
  assert.equal(inspection.stateCount, view.state_count);
  assert.equal(inspection.definitionBytes, view.definition_bytes);
  assert.equal(inspection.binding, 'early');
  assert.equal(inspection.retired, false);
  // The addon charges no native memory of its own to definitions.
  const { memory } = system.stats();
  for (const name of ['envelopes', 'scheduling', 'values', 'directory']) {
    assert.equal(memory[name].bytesInUse, 0, `${name} holds nothing for a definition`);
  }
  system.close();
}

// Where each element was written (core ADR 0085): the builder records the
// file, line and column of every call that writes a state, transition,
// initial transition or action, core keeps them per definition, and
// sourceOf() reads them back by core's IDs.
function sourceSpans() {
  const system = createSystem();
  // The line of this statement; the chart's calls are written below it, one
  // per statement, because engines place a call inside a chain differently.
  const line = Number(new Error().stack.split('\n')[1].match(/:(\d+):\d+\)?$/)[1]);
  const chart = statechart({ family: 'spans', revision: 'v1' });
  chart.state('idle', (idle) => {
    idle.on('go', (go) => {
      go.raise('started');
      go.target('busy');
    });
  });
  chart.state('busy', (busy) => {
    busy.initial('inner', (actions) => {
      actions.log('entering');
    });
    busy.state('inner');
  });
  chart.script(() => {});
  const spec = chart.build();
  assert.deepEqual(spec.sources, [{ name: __filename }]);
  const definition = system.define(spec);
  const idle = definition.sourceOf('state', 0);
  assert.equal(idle.source, __filename);
  assert.equal(idle.line, line + 2);
  // `chart.state(`: V8 points at the method's name, JavaScriptCore at the dot.
  assert.ok([8, 9].includes(idle.column), `column ${idle.column}`);
  assert.equal(definition.sourceOf('state', 1).line, line + 8);
  assert.equal(definition.sourceOf('state', 2).line, line + 12);
  // The <transition>, then busy's initial transition; the document's is SCXML's default.
  assert.equal(definition.sourceOf('transition', 0).line, line + 3);
  assert.equal(definition.sourceOf('transition', 1).line, line + 9);
  assert.equal(definition.sourceOf('transition', 2), undefined);
  const lines = [0, 1, 2].map((id) => definition.sourceOf('action', id)?.line).sort();
  assert.deepEqual(lines, [line + 4, line + 10, line + 14].sort(), 'raise, log and the root script');
  // A function is a host expression; the builder gives expressions no span of their own.
  assert.equal(definition.sourceOf('expression', 0), undefined);
  assert.equal(definition.sourceOf('state', 9), undefined);
  // A spec without spans compiles as before and keeps none.
  const plain = system.define({ identity: { family: { name: 'plain' }, revision: 'v1' }, states: [{ kind: 'atomic', id: 'a' }] });
  assert.equal(plain.sourceOf('state', 0), undefined);
  // A span that gives only a line: no column, and no end column, rather
  // than a column 0 that SourcePlace's count from 1 does not have.
  const lined = system.define({
    identity: { family: { name: 'lined' }, revision: 'v1' },
    sources: [{ name: 'chart.txt' }],
    spans: [{ source: 0, line: 3 }, { source: 0, line: 5, column: 7, end_line: 6 }],
    states: [{ kind: 'atomic', id: 'a', span: 1 }, { kind: 'atomic', id: 'b', span: 2 }],
  });
  assert.deepEqual(lined.sourceOf('state', 0), { source: 'chart.txt', line: 3 });
  assert.deepEqual(lined.sourceOf('state', 1), { source: 'chart.txt', line: 5, column: 7, endLine: 6 });
  system.close();
}

// The states of a spec in document order, which is how core numbers them,
// with each one's parent's index.
function specStates(spec) {
  const out = [];
  const walk = (states, parent) => {
    for (const state of states ?? []) {
      const index = out.length;
      out.push({ state, parent });
      walk(state.states, index);
    }
  };
  walk(spec.states, null);
  return out;
}

// Every lookup agrees with the export (napi ADR 0027 point 1): states by
// document order, <transition>s with their source, each action at the
// place its block says, conditions and content as the spec holds them.
function assertLookupsAgree(definition) {
  const spec = definition.export();
  const view = definition.inspect();
  const states = specStates(spec);
  assert.equal(view.stateCount, states.length);
  states.forEach(({ state, parent }, id) => {
    assert.deepEqual(definition.state(id), { id, name: state.id, parent, kind: state.kind });
    assert.equal(definition.findState(state.id).id, id);
  });
  const authored = states.flatMap(({ state }, source) => (state.transitions ?? []).map((transition) => ({ transition, source })));
  for (let id = 0; id < view.transitionCount; id += 1) {
    const found = definition.transition(id);
    assert.equal(found.id, id);
    if (found.origin !== 'authored') {
      continue;
    }
    const { transition, source } = authored[found.documentOrder];
    assert.equal(found.source, source);
    assert.equal(found.event, transition.event);
    assert.deepEqual(found.targets.map((target) => definition.state(target).name), transition.targets ?? []);
    assert.equal(found.actionCount, (transition.actions ?? []).length);
    if (transition.condition === undefined) {
      assert.equal(found.condition, null);
    } else {
      // The export's source text carries its span ({ source, span }); a
      // lookup gives the text, as the definition keeps it.
      const condition = transition.condition;
      assert.equal(definition.expression(found.condition), typeof condition?.source === 'string' ? condition.source : condition);
    }
  }
  const actionsOf = (action) => {
    const found = definition.action(action);
    switch (found.block) {
      case 'onentry': return states[found.owner].state.onentry[found.blockIndex].actions;
      case 'onexit': return states[found.owner].state.onexit[found.blockIndex].actions;
      case 'finalize': return states[found.owner].state.invokes[found.blockIndex].finalize;
      case 'script': return [{ kind: 'script' }];
      case 'transition': {
        const transition = definition.transition(found.owner);
        if (transition.origin === 'authored') return authored[transition.documentOrder].transition.actions;
        if (transition.origin === 'bootstrap') return spec.initial.actions;
        return states[transition.source].state.initial.actions;
      }
      default: {
        const owner = actionsOf(found.owner)[definition.action(found.owner).index];
        if (found.block === 'then') return owner.conditional.then_actions;
        if (found.block === 'else') return owner.conditional.else_actions;
        return owner.foreach.body;
      }
    }
  };
  for (let id = 0; id < view.actionCount; id += 1) {
    const found = definition.action(id);
    assert.equal(found.id, id);
    assert.equal(actionsOf(id)[found.index].kind, found.kind, `action ${id}`);
  }
  for (let id = 0; id < view.expressionCount; id += 1) {
    assert.notEqual(definition.expression(id), undefined, `expression ${id}`);
  }
  for (let id = 0; id < view.contentCount; id += 1) {
    assert.ok(definition.content(id).bytes instanceof Uint8Array);
  }
  const nested = [];
  for (let index = 0; index < view.invokeCount; index += 1) {
    const found = definition.invoke(index);
    assert.equal(found.index, index);
    if (found.nested !== null) nested.push(found.nested);
  }
  assert.deepEqual(nested, view.nested ?? []);
  // Out of range is a question, not an error.
  assert.equal(definition.state(view.stateCount), undefined);
  assert.equal(definition.state(-1), undefined);
  assert.equal(definition.state(0.5), undefined);
  assert.equal(definition.transition(view.transitionCount), undefined);
  assert.equal(definition.action(view.actionCount), undefined);
  assert.equal(definition.expression(view.expressionCount), undefined);
  assert.equal(definition.content(view.contentCount), undefined);
  assert.equal(definition.invoke(view.invokeCount), undefined);
  assert.equal(definition.findState('no such state'), undefined);
}

const ORDER = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="order" initial="open" datamodel="null">
  <state id="open">
    <onentry><log label="opened"/><if cond="In('open')"><raise event="checked"/><else/><raise event="odd"/></if></onentry>
    <transition event="pay" cond="In('open')" target="paid"><send event="receipt" target="#_parent"><content>thanks</content></send></transition>
    <invoke type="scxml"><content><scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="clerk" datamodel="null"><final id="done"/></scxml></content>
      <finalize><log label="clerk done"/></finalize></invoke>
  </state>
  <state id="paid" initial="packing">
    <state id="packing"><transition event="packed" target="shipped"/></state>
    <final id="shipped"/>
  </state>
</scxml>`;

// An imported chart has no functions: defining its export answers it
// (core ADR 0077), and the lookups agree with the export.
function exportedDocument() {
  const system = createSystem();
  const order = system.define(ORDER, { format: 'scxml', name: 'file:///charts/order.scxml' });
  const spec = order.export();
  assert.equal(spec.identity, undefined, 'an imported chart is fingerprinted');
  assert.deepEqual(spec.sources, [{ name: 'file:///charts/order.scxml' }]);
  const again = system.define(spec);
  assert.equal(again.id, order.id);
  assert.deepStrictEqual(again.export(), spec);
  assertLookupsAgree(order);
  const view = order.inspect();
  assert.equal(view.invokeCount, 1);
  assert.deepEqual(order.transition(0), {
    id: 0, origin: 'authored', source: 0, type: 'external', event: 'pay', targets: [1],
    defaultTargets: false, condition: order.transition(0).condition, actionCount: 1, documentOrder: 0,
  });
  assert.equal(order.expression(order.transition(0).condition), "In('open')");
  const pay = spec.states[0].transitions[0].condition;
  assert.equal(pay.source, "In('open')");
  assert.equal(typeof pay.span, 'number', 'source text keeps its span through the export');
  const bootstrap = order.transition(view.transitionCount - 1);
  assert.equal(bootstrap.origin, 'bootstrap');
  assert.equal(bootstrap.source, null);
  assert.equal(bootstrap.event, undefined);
  const receipt = [...Array(view.contentCount).keys()].map((id) => order.content(id))
    .find((content) => new TextDecoder().decode(content.bytes) === 'thanks');
  assert.equal(receipt.mediaType, 'application/scxml+xml');
  const invoke = order.invoke(0);
  assert.equal(invoke.type, 'http://www.w3.org/TR/scxml/');
  assert.equal(invoke.src, null);
  assert.match(new TextDecoder().decode(order.content(invoke.content).bytes), /name="clerk"/);
  const clerk = [...system.definitions()].find((definition) => definition.id === invoke.nested);
  assert.equal(clerk.identity.family, 'clerk');
  system.close();
}

// Defining a builder chart's export answers the chart itself (napi ADR 0030,
// core ADR 0087): the export hands back the same functions, literal() values
// and locations, which are the same handles, and the same spans.
function exportedBuilder() {
  const system = createSystem();
  const whenPositive = function () { return this.count > 0; };
  const location = { get() { return this.count; }, set(value) { this.count = value; } };
  const counter = system.define(statechart({ family: 'counter', revision: 'v1' })
    .data('count', 0)
    .initial('idle')
    .state('idle', (state) => state
      .on('add', (t) => t.assign('count', function () { return this.count + 1; }))
      .on('go', (t) => t.when(whenPositive).target('busy').assignAt(location, literal(5))))
    .state('busy'));
  const spec = counter.export();
  assert.deepEqual(spec.identity.family, { name: 'counter' });
  const again = system.define(spec);
  assert.equal(again.id, counter.id);
  assert.deepStrictEqual(again.export(), spec);
  assertLookupsAgree(counter);
  const go = [0, 1].map((id) => counter.transition(id)).find((transition) => transition.event === 'go');
  assert.equal(counter.expression(go.condition), whenPositive);
  const expressions = [...Array(counter.inspect().expressionCount).keys()].map((id) => counter.expression(id));
  assert.ok(expressions.includes(location), 'a location is the object the chart was built with');
  assert.ok(expressions.some((expression) => expression?.value === 5), 'a literal() keeps its value');
  system.close();
}

// A freed definition throws `not-found` from every lookup and the export.
function freed() {
  const system = createSystem();
  const definition = system.define({
    identity: { family: { name: 'gone' }, revision: 'v1' },
    states: [{ kind: 'atomic', id: 'a' }],
  });
  assert.equal(definition.state(0).name, 'a');
  definition.retire();
  for (const read of [() => definition.export(), () => definition.state(0), () => definition.state(5),
    () => definition.findState('a'), () => definition.transition(0), () => definition.action(0),
    () => definition.expression(0), () => definition.content(0), () => definition.invoke(0)]) {
    assertCode(read, 'not-found');
  }
  system.close();
}

// A spec names any data model (napi ADR 0027): an imported chart on a
// registered data model defines again from its export, in its own system
// and in another that registers the same name; a name nothing registered
// is refused.
async function specOnRegisteredDatamodel() {
  const text = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="tally" datamodel="ecmascript" initial="counting">
    <datamodel><data id="count" expr="1"/></datamodel>
    <state id="counting">
      <transition event="add" cond="count &lt; 3" target="counting"><assign location="count" expr="count + 1"/></transition>
      <transition event="add" cond="count >= 3" target="full"/>
    </state>
    <final id="full"/>
  </scxml>`;
  const withEcmascript = () => {
    const system = createSystem();
    system.registerDatamodel('ecmascript', ecmascript);
    return system;
  };
  const first = withEcmascript();
  const tally = first.define(text, { format: 'scxml' });
  const spec = tally.export();
  assert.equal(spec.datamodel, 'ecmascript');
  assert.equal(first.define(spec).id, tally.id, 'the same system answers the same definition');
  assert.equal(tally.inspect().datamodel, 'ecmascript');

  const second = withEcmascript();
  const copy = second.define(spec);
  assert.equal(copy.inspect().datamodel, 'ecmascript');
  assert.deepStrictEqual(copy.export(), spec);
  const actor = second.spawn(copy);
  await actor.send('add');
  await actor.send('add');
  assert.deepEqual(actor.inspect().activeStates, ['counting'], 'count < 3 held twice');
  await actor.send('add');
  assertCode(() => actor.inspect(), 'not-found', undefined, 'count >= 3 finished it, and it was reclaimed');

  const bare = createSystem();
  assertCode(() => bare.define(spec), 'invalid', /no data model is named "ecmascript"/);
  // The refusal leaves the system usable: the next spec compiles as its own.
  assert.equal(bare.define({ name: 'plain', states: [{ kind: 'atomic', id: 'a' }] }).inspect().datamodel, 'javascript');
  for (const system of [first, second, bare]) {
    system.close();
  }
}

const KID = '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="kid" datamodel="null"><final id="f"/></scxml>';
const PARENT = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="parent" datamodel="null">
  <state id="a">
    <invoke type="scxml" src="kid.scxml"/>
    <invoke type="scxml"><content><scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="inline" datamodel="null"><final id="f"/></scxml></content></invoke>
  </state>
</scxml>`;

const familiesOf = (system, ids) => ids.map((id) => [...system.definitions()].find((view) => view.id === id).identity.family);

// define(spec) walks inline SCXML children as an import does (napi ADR
// 0027, ADR 0024): another system defining a parent's export nests its
// children, a literal src only when `resolve` is given, and a broken child
// refuses the parent at define time and leaves nothing behind.
function specChildren() {
  const source = createSystem();
  const parent = source.define(PARENT, { format: 'scxml', resolve: () => KID });
  const spec = parent.export();
  assert.deepEqual(familiesOf(source, parent.inspect().nested), ['kid', 'inline']);

  const resolved = createSystem();
  const withResolve = resolved.define(spec, { resolve: (src) => (src === 'kid.scxml' ? KID : undefined) });
  assert.deepEqual(familiesOf(resolved, withResolve.inspect().nested), ['kid', 'inline']);

  const unresolved = createSystem();
  const withoutResolve = unresolved.define(spec);
  assert.deepEqual(familiesOf(unresolved, withoutResolve.inspect().nested), ['inline'],
    'a literal src without resolve is fetched at invoke time, as for an import');

  const broken = structuredClone(spec);
  broken.identity = undefined;
  broken.name = 'broken';
  broken.states[0].invokes[1].data.content.bytes = new TextEncoder().encode('<scxml name="bad"><state');
  const refusing = createSystem();
  assertCode(() => refusing.define(broken), 'invalid', /child chart of <invoke> 1/);
  assert.deepEqual([...refusing.definitions()], [], 'the refused parent and its children are freed');
  assertCode(() => refusing.define(spec, { resolve: 'kid.scxml' }), 'invalid', /resolve must be a function/);
  for (const system of [source, resolved, unresolved, refusing]) {
    system.close();
  }
}

definitions();
builder();
refusals();
nothingStored();
sourceSpans();
exportedDocument();
exportedBuilder();
freed();
specChildren();
specOnRegisteredDatamodel().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
