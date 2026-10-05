'use strict';

// Mail a chart sends to a target that does not exist (napi ADR 0022): the
// sender gets error.communication (SCXML 6.2.4), which core raises itself
// when the host cannot load the address (core ADR 0099) and which carries no
// data, while mail posted from JavaScript stays a dead letter (ADR 0008), and
// a family with a loader still loads. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

// A chart that sends `ping` to `target` on `go` (after `delay` ms when given)
// and counts the error.communication events it gets.
function sender(system, family, target, delay) {
  return system.define(
    // Each target and delay is another chart, so another revision (core ADR 0087).
    statechart({ family, revision: `v1 ${target} ${delay}` })
      .data('failures', 0)
      .state('idle', (s) => {
        s.on('go', (t) =>
          t.send('ping', (m) => {
            m.to(target);
            if (delay !== undefined) m.after(delay);
          }),
        );
        s.on('error.communication', (t) =>
          t.assign('failures', function () { return this.failures + 1; }),
        );
      }),
  );
}

function receiver(system, family) {
  return system.define(
    statechart({ family, revision: 'v1' })
      .data('pings', 0)
      .state('idle', (s) => s.on('ping', (t) => t.assign('pings', function () { return this.pings + 1; }))),
  );
}

function sum(system, name, attributes = {}) {
  const metric = system.metrics().metrics.find((m) => m.name === name);
  return (metric?.points ?? [])
    .filter((p) => Object.entries(attributes).every(([k, v]) => p.attributes[k] === v))
    .reduce((total, p) => total + p.value, 0);
}

// An unknown name in a family with no loader: the chart gets the error, and
// JavaScript's post to the same name is still a dead letter.
async function unknownName() {
  const dead = [];
  const system = createSystem({ deadLetter: (record) => dead.push(record) });
  const actor = system.spawn(sender(system, 'sender', '#_actor_nobody/here'));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.equal(actor.inspect().data.failures, 1, 'the chart got error.communication once');
  assert.equal(dead.length, 0, 'the chart\'s mail is not a dead letter');
  assert.equal(system.metrics().system.errors_communication, 1);
  assert.equal(sum(system, 'tinyactors.messages.dead_lettered'), 0);

  system.post({ family: 'nobody', name: 'here' }, 'ping');
  await system.flush();
  assert.equal(dead.at(-1)?.reason, 'not-found', 'mail from JavaScript stays a dead letter');
  system.close();
}

// A session ID that no actor has any more: the chart gets the error.
async function unknownSession() {
  const system = createSystem();
  const gone = system.spawn(receiver(system, 'receiver'));
  await system.flush();
  const session = gone.inspect().sessionID;
  system.destroy(gone);
  await system.flush();
  const actor = system.spawn(sender(system, 'sender', `#_scxml_${session}`));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.equal(actor.inspect().data.failures, 1);
  system.close();
}

// A family with a loader: an unknown name loads. The loader's "not-found"
// fails the load, and core recycles the mail the chart sent: the sender gets
// error.communication, as for any address nobody can serve, and no dead
// letter (those are mail posted from JavaScript).
async function loaderFamily() {
  const dead = [];
  const system = createSystem({ deadLetter: (record) => dead.push(record) });
  const loaded = [];
  const definition = receiver(system, 'lazy');
  system.setLoader('lazy', async (address) => {
    loaded.push(address.name);
    return address.name === 'known' ? { spawn: definition } : 'not-found';
  });
  const known = system.spawn(sender(system, 'sender', '#_actor_lazy/known'), { name: 'k' });
  const unknown = system.spawn(sender(system, 'sender', '#_actor_lazy/unknown'), { name: 'u' });
  await system.flush();
  known.post('go');
  unknown.post('go');
  await system.flush();
  assert.deepEqual(loaded.sort(), ['known', 'unknown']);
  assert.equal(known.inspect().data.failures, 0);
  assert.equal(unknown.inspect().data.failures, 1, 'a loader that cannot find the name is the sender\'s error');
  assert.equal(dead.find((record) => record.event === 'ping'), undefined, 'the chart\'s mail is not a dead letter');
  system.close();
}

// A delayed send whose target is destroyed before it comes due: the error
// reaches the sender when the timer fires.
async function delayedSendToADestroyedTarget() {
  const system = createSystem({ clock: 'manual' });
  const target = system.spawn(receiver(system, 'receiver'), { name: 'r' });
  const actor = system.spawn(sender(system, 'sender', '#_actor_receiver/r', 100));
  await system.flush();
  actor.post('go');
  await system.flush();
  system.destroy(target);
  await system.flush();
  assert.equal(actor.inspect().data.failures, 0, 'nothing is due yet');
  await system.advanceTo(system.time + 200);
  assert.equal(actor.inspect().data.failures, 1, 'the error reached the sender when the timer fired');
  system.close();
}

// A resident target still gets its mail.
async function residentTarget() {
  const system = createSystem();
  const target = system.spawn(receiver(system, 'receiver'), { name: 'r' });
  const actor = system.spawn(sender(system, 'sender', '#_actor_receiver/r'));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.equal(target.inspect().data.pings, 1);
  assert.equal(actor.inspect().data.failures, 0);
  system.close();
}

(async () => {
  await unknownName();
  await unknownSession();
  await loaderFamily();
  await delayedSendToADestroyedTarget();
  await residentTarget();
  console.log('unreachable.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
