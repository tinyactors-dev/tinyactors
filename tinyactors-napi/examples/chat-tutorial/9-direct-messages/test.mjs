import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run, start } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

/** A host with `names` signed in. */
async function signedIn(datamodel, names) {
  const chat = await start({ datamodel });
  const users = names.map((name) => chat.host.spawn('user', { name: `user.${name}`, params: { name } }));
  for (const user of users) user.send('connect');
  await chat.host.idle();
  return { ...chat, users };
}

for (const datamodel of ['native', 'quickjs']) {
  test(`9-direct-messages, ${datamodel}: a DM is made once per pair, read on sign-in, and closed to a third person`, async () => {
    const lines = await run({ datamodel });
    assert.deepEqual(phases(lines), phases([
      '-- scout opens a DM with forager, who is signed out, and posts twice',
      'scout opens dm.forager.scout, the DM with forager, and reads nothing yet',
      'scout sees post 1 in the DM with forager from scout at 0:00: sugar by the oak',
      'scout sees post 2 in the DM with forager from scout at 0:30: can you carry?',
      '-- forager signs in, types and answers',
      'forager has dm.forager.scout, the DM with scout, and reads scout at 0:00: sugar by the oak | scout at 0:30: can you carry?',
      'scout sees forager typing in the DM with forager',
      'forager sees post 3 in the DM with scout from forager at 1:00: on my way',
      'scout sees post 3 in the DM with forager from forager at 1:00: on my way',
      '-- forager opens the DM with scout: the same one',
      'forager opens dm.forager.scout, the DM with scout, and reads scout at 0:00: sugar by the oak | scout at 0:30: can you carry? | forager at 1:00: on my way',
      '-- queen tries to read it',
      'queen gets an error: not a member',
      'the channels: general',
    ]));
    assert.ok(!lines.some((line) => line.includes('let me in')), 'a post from a third person reaches nobody');
  });

  test(`9-direct-messages, ${datamodel}: two people who open their DM at the same moment get one`, async () => {
    const { host, client, users: [scout, forager] } = await signedIn(datamodel, ['scout', 'forager']);
    scout.send('dm.open', { with: 'forager' });
    forager.send('dm.open', { with: 'scout' });
    await host.idle();
    assert.deepEqual(client.lines.filter((line) => line.includes(' opens ')).sort(), [
      'forager opens dm.forager.scout, the DM with scout, and reads nothing yet',
      'scout opens dm.forager.scout, the DM with forager, and reads nothing yet',
    ]);
    assert.equal((await host.actors({ chart: 'room' })).length, 2); // general and the DM
    host.close();
  });

  test(`9-direct-messages, ${datamodel}: a DM holds posts through a lost connection, and one with oneself reaches oneself once`, async () => {
    const { host, client, clock, users: [scout, forager] } = await signedIn(datamodel, ['scout', 'forager']);
    scout.send('dm.open', { with: 'forager' });
    scout.send('dm.open', { with: 'scout' });
    await host.idle();
    forager.send('connection.lost');
    await host.idle();
    scout.send('post', { room: 'dm.forager.scout', text: 'in the tunnel?' });
    scout.send('post', { room: 'dm.scout', text: 'a note' });
    await host.idle();
    clock.increment(10_000);
    forager.send('connect');
    await host.idle();
    assert.deepEqual(client.lines.filter((line) => line.includes(' sees post ')).sort(), [
      'forager sees post 1 in the DM with scout from scout at 0:00: in the tunnel?',
      'scout sees post 1 in the DM with forager from scout at 0:00: in the tunnel?',
      'scout sees post 1 in the DM with scout from scout at 0:00: a note',
    ]);
    host.close();
  });
}
