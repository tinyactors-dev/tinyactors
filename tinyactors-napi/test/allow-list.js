'use strict';

// Granary's allow list (kernel ADR 0006, the napi README's "Persistence by
// replay"): the people who may manage pull requests, journaled by the
// recorder and rebuilt from the journal in another system. While it is
// rebuilt nothing leaves it: no I/O processor, no invoker, no mail to
// another actor, until it is published. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

// grant and revoke change the list; each change is written to the audit
// I/O processor and told to the notifier actor. While open, the list
// watches the directory service (an invocation), which can revoke people.
const allowList = () => statechart({ family: 'allow-list', revision: 'v1' })
  .data('people', [])
  .state('open', (s) => s
    .invoke((i) => i.type('directory').id('dir'))
    .on('grant', (t) => t
      .when(({ data, event }) => !data.people.includes(event.data.person))
      .assign('people', ({ data, event }) => [...data.people, event.data.person])
      .send('granted', (send) => send.via('audit').data(({ event }) => ({ person: event.data.person, by: event.data.by })))
      .send('changed', (send) => send.to('#_actor_notifier/main').data(({ data }) => ({ people: data.people }))))
    .on('revoke', (t) => t
      .assign('people', ({ data, event }) => data.people.filter((person) => person !== event.data.person))
      .send('revoked', (send) => send.via('audit').data(({ event }) => ({ person: event.data.person, by: event.data.by })))
      .send('changed', (send) => send.to('#_actor_notifier/main').data(({ data }) => ({ people: data.people }))))
    .on('freeze', (t) => t.target('frozen')))
  .state('frozen', (s) => s.on('thaw', (t) => t.target('open')));

const notifier = () => statechart({ family: 'notifier', revision: 'v1' })
  .data('changes', 0)
  .state('listening', (s) => s.on('changed', (t) => t.assign('changes', ({ data }) => data.changes + 1)));

// A system with the audit processor, the directory invoker and the
// notifier, counting what crosses into each.
function world(initialTime = 0) {
  const crossed = { audit: [], started: 0 };
  let directory;
  const system = createSystem({
    clock: 'manual',
    initialTime,
    invokers: { directory: { start(_, report) { crossed.started += 1; directory = report; } } },
  });
  system.registerIO('audit', { send(request) { crossed.audit.push(request.data); } });
  const definition = system.define(allowList());
  const notifierActor = system.spawn(system.define(notifier()), { name: 'main' });
  return { system, definition, notifierActor, crossed, directory: () => directory };
}

async function main() {
  // Live: the recorder journals the list's events as JSON.
  const journal = [];
  const live = world();
  live.system.setRecorder(({ records }) => {
    for (const { address, event, at } of records) {
      if (address.family === 'allow-list') journal.push(JSON.parse(JSON.stringify({ event, at })));
    }
  }, { families: ['allow-list'] });
  const list = live.system.spawn(live.definition, { name: 'pull-requests' });
  await live.system.flush();
  await live.system.advanceTo(1000);
  await list.send('grant', { person: 'alice', by: 'owner' });
  await live.system.advanceTo(2000);
  await list.send('grant', { person: 'bob', by: 'alice' });
  await live.system.advanceTo(3000);
  live.directory().event('revoke', { person: 'alice', by: 'directory' }); // the service's own event
  await live.system.flush();
  assert.deepEqual(list.inspect().data.people, ['bob']);
  assert.equal(live.crossed.audit.length, 3);
  assert.equal(live.crossed.started, 1);
  assert.equal(live.notifierActor.inspect().data.changes, 3);
  live.system.close();

  // The journal explains the list: who was granted or revoked, by whom, when.
  assert.deepEqual(journal.map(({ event, at }) => [event.name, event.data.person, event.data.by, at]), [
    ['grant', 'alice', 'owner', 1000],
    ['grant', 'bob', 'alice', 2000],
    ['revoke', 'alice', 'directory', 3000],
  ]);
  assert.equal(journal[2].event.invocationID, 'dir', 'the revoke came from the directory service');

  // Rebuilt in another system: the same list, and nothing crossed before publish.
  const again = world(10_000);
  const prepared = again.system.prepare(again.definition, { address: { family: 'allow-list', name: 'pull-requests' }, at: 0 });
  for (const { event, at } of journal) prepared.replay(event, at);
  await again.system.flush();
  assert.deepEqual(prepared.inspect().data.people, ['bob']);
  assert.deepEqual(again.crossed.audit, [], 'no I/O processor was called');
  assert.equal(again.crossed.started, 0, 'no invoker was called');
  assert.equal(again.notifierActor.inspect().data.changes, 0, 'no mail reached another actor');

  // Published: an ordinary actor. Its invocation did not survive, and the
  // chart hears it; from here on, what it does crosses.
  const rebuilt = again.system.publish(prepared);
  await again.system.flush();
  assert.equal(again.crossed.started, 0, 'publish retires the invocation instead of starting it');
  await rebuilt.send('grant', { person: 'carol', by: 'bob' });
  await again.system.flush();
  assert.deepEqual(rebuilt.inspect().data.people, ['bob', 'carol']);
  assert.deepEqual(again.crossed.audit, [{ person: 'carol', by: 'bob' }]);
  assert.equal(again.notifierActor.inspect().data.changes, 1);
  again.system.close();
  console.log('allow-list: ok');
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
