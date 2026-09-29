'use strict';

// Session IDs (core ADR 0088, napi ADR 0032 section 1): a system's
// incarnation, and a session ID shown as text, <incarnation>.<session>.<family>,
// everywhere napi shows one: inspection, captures, the evaluation context,
// and the OTLP attributes of spans, including a cause from another family.
// Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, decodeTraces, stepSpanID, TinyactorsError } = require('..');

function incarnations() {
  const first = createSystem();
  const second = createSystem();
  assert.equal(typeof first.incarnation, 'bigint');
  assert.notEqual(first.incarnation, second.incarnation, 'the default is random');
  first.close();
  second.close();
  for (const [given, expected] of [[0, 0n], [5, 5n], [2n ** 64n - 1n, 2n ** 64n - 1n]]) {
    const system = createSystem({ incarnation: given });
    assert.equal(system.incarnation, expected);
    system.close();
  }
  for (const given of [-1, 1.5, 2n ** 64n, -1n, 'x', Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(() => createSystem({ incarnation: given }),
                  (error) => error instanceof TinyactorsError && error.code === 'invalid', String(given));
  }
}

async function shownAsText() {
  const system = createSystem({ incarnation: 42 });
  let seen;
  const definition = system.define(
    statechart({ family: 'shown/as.text', revision: 'v1' })
      .state('idle', (s) => s.on('look', (t) => t.script(function ({ sessionID }) { seen = sessionID; }))),
  );
  const actor = system.spawn(definition);
  await system.flush();
  actor.post('look');
  await system.flush();
  const view = actor.inspect();
  assert.equal(view.sessionID, `42.${view.step.session}.shown/as.text`, 'a family may hold / and .');
  assert.equal(seen, view.sessionID, 'what the chart reads as _sessionid');
  const image = actor.capture();
  assert.equal(image.sessionID, view.sessionID);
  image.destroy();
  system.close();
}

// A span names its session, and its cause's session, as text: the cause is
// the sending step of an actor of another family.
async function tracesNameSessionsAsText() {
  const system = createSystem({ incarnation: 7 });
  const batches = [];
  system.setTraceSink((bytes) => batches.push(bytes), { detail: 'full' });
  const replier = system.spawn(system.define(
    statechart({ family: 'replier', revision: 'v1' }).state('idle', (s) => s.on('ping', (t) => t.target('idle'))),
  ));
  await system.flush();
  const target = `#_scxml_${replier.inspect().sessionID}`;
  const asker = system.spawn(system.define(
    statechart({ family: 'asker', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.send('ping', (send) => send.to(target)))),
  ));
  await system.flush();
  asker.post('go');
  await system.flush();
  await system.flush();
  const spans = batches.flatMap((bytes) => decodeTraces(bytes));
  const ping = spans.find((span) => span.name === 'scxml.macrostep ping');
  assert.ok(ping, `spans: ${spans.map((span) => span.name)}`);
  assert.equal(ping.attributes['scxml.session_id'], replier.inspect().sessionID);
  assert.equal(ping.attributes['scxml.cause.session_id'], asker.inspect().sessionID);
  const cause = ping.attributes;
  assert.equal(ping.parentSpanID, stepSpanID(cause['scxml.cause.session_id'], cause['scxml.cause.macrostep'],
                                             cause['scxml.cause.microstep']),
               'stepSpanID takes a session ID as text');
  system.close();
}

(async () => {
  incarnations();
  await shownAsText();
  await tracesNameSessionsAsText();
  console.log('sessions.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
