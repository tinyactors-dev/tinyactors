'use strict';

// Mail for a session that has gone goes to its family's loader (napi ADR
// 0032 section 2, core ADR 0088): a reply to a finished requester starts a
// new instance of its family, which gets the reply; without a loader the
// sender gets error.communication, as napi ADR 0022 says, and so does the
// sender of mail the loader answers "not-found" for (core ADR 0099); a session of another incarnation goes to the loader too;
// several messages for one session wait on one load. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

// Turns until `done()` holds: loaders settle between turns.
async function until(system, done, what) {
  for (let turn = 0; turn < 200; turn += 1) {
    await system.flush();
    if (done()) {
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail(`timed out waiting for ${what}`);
}

// The server replies `replies` times to whoever asked, and counts the
// error.communication events it gets about a reply.
function server(system, replies = 1) {
  return system.define(
    statechart({ family: 'server', revision: `v${replies}` })
      .data('failures', 0)
      .state('idle', (s) => {
        s.on('ask', (t) => {
          for (let at = 0; at < replies; at += 1) {
            t.send('reply', (m) => m.to(({ event }) => event.origin).data(() => ({ at })));
          }
        });
        s.on('error.communication', (t) => t.assign('failures', function () { return this.failures + 1; }));
      }),
  );
}

// A requester asks the server, then finishes at once (and is destroyed).
function requester(system, target = '#_actor_server/s') {
  return system.define(
    statechart({ family: 'requester', revision: `ask ${target}` })
      .state('asking', (s) => s.entry((e) => e.send('ask', (m) => m.to(target))).always((t) => t.target('done')))
      .final('done'),
  );
}

// What a loader starts for a requester that has gone: it keeps the replies.
function catcher(system, caught) {
  return system.define(
    statechart({ family: 'requester', revision: 'catcher' })
      .state('waiting', (s) => s.on('reply', (t) => t.script(({ event, sessionID }) => {
        caught.push({ data: event.data, sessionID });
      }))),
  );
}

async function replyToAGoneRequester() {
  const system = createSystem({ incarnation: 3 });
  const caught = [];
  const calls = [];
  system.setLoader('requester', (target, why) => {
    calls.push({ target, why });
    return { spawn: catcher(system, caught) };
  });
  const s = system.spawn(server(system), { address: { family: 'server', name: 's' } });
  const asker = system.spawn(requester(system));
  const gone = asker.inspect().sessionID;
  await until(system, () => caught.length === 1, 'the reply');
  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0], { target: { family: 'requester', sessionID: gone }, why: 'session' });
  assert.deepEqual(caught[0].data, { at: 0 });
  assert.notEqual(caught[0].sessionID, gone, 'a new instance, a new session');
  assert.equal(s.inspect().data.failures, 0, 'the server\'s reply was not refused');
  system.close();
}

async function withoutALoader() {
  const system = createSystem();
  const s = system.spawn(server(system), { address: { family: 'server', name: 's' } });
  system.spawn(requester(system));
  await until(system, () => s.inspect().data.failures === 1, 'the refusal');
  system.close();
}

async function notFoundIsAnError() {
  const dead = [];
  const system = createSystem({ incarnation: 0, deadLetter: (record) => dead.push(record) });
  system.setLoader('requester', () => 'not-found');
  const s = system.spawn(server(system), { address: { family: 'server', name: 's' } });
  const asker = system.spawn(requester(system));
  const gone = asker.inspect().sessionID;
  await until(system, () => s.inspect().data.failures === 1, 'the refusal');
  assert.equal(dead.length, 0, 'the server\'s mail is not a dead letter: it is the server\'s error');
  assert.equal(typeof gone, 'string');
  system.close();
}

async function anotherIncarnation() {
  const system = createSystem({ incarnation: 11 });
  const caught = [];
  const calls = [];
  system.setLoader('requester', (target, why) => {
    calls.push({ target, why });
    return { spawn: catcher(system, caught) };
  });
  const sender = system.spawn(system.define(
    statechart({ family: 'sender', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.send('reply', (m) => m.to('#_scxml_999.5.requester').data(() => 'late')))),
  ));
  await system.flush();
  sender.post('go');
  await until(system, () => caught.length === 1, 'the reply');
  assert.deepEqual(calls, [{ target: { family: 'requester', sessionID: '11.0.requester' }, why: 'session' }]);
  assert.equal(caught[0].data, 'late');
  system.close();
}

async function oneLoadForSeveralMessages() {
  const system = createSystem();
  const caught = [];
  let calls = 0;
  let release;
  const ready = new Promise((resolve) => { release = resolve; });
  system.setLoader('requester', async () => {
    calls += 1;
    await ready;
    return { spawn: catcher(system, caught) };
  });
  system.spawn(server(system, 3), { address: { family: 'server', name: 's' } });
  system.spawn(requester(system));
  // flush() waits for loads in flight, so let turns run on a timer instead.
  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.equal(calls, 1, 'one load for the session');
  release();
  await until(system, () => caught.length === 3, 'the replies');
  assert.deepEqual(caught.map((entry) => entry.data.at), [0, 1, 2], 'in order');
  assert.equal(new Set(caught.map((entry) => entry.sessionID)).size, 1, 'one instance got them all');
  system.close();
}

(async () => {
  await replyToAGoneRequester();
  await withoutALoader();
  await notFoundIsAnError();
  await anotherIncarnation();
  await oneLoadForSeveralMessages();
  console.log('session-loads.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
