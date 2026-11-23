// The three starting audiences: put in the store once, none active, and a
// deleted one stays deleted.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup } from './fakes.mjs';
import { STARTER_AUDIENCES } from '../src/core/audiences.mjs';
import { createMemoryStore } from '../src/core/store.mjs';

test('the starting audiences are there on the first start, none active; one deleted stays deleted', async () => {
  const store = createMemoryStore();
  const first = await setup({ store, starterAudiences: STARTER_AUDIENCES });
  const names = first.reviewer.getState().settings.audiences.map((a) => a.name);
  assert.deepEqual(names, STARTER_AUDIENCES.map((a) => a.name).sort());
  assert.deepEqual(first.reviewer.getState().settings.audienceIDs, []);
  for (const a of first.reviewer.getState().settings.audiences) assert.ok(a.wants && a.tasks && a.objections, a.name);
  const gone = STARTER_AUDIENCES[0].id;
  await store.deleteAudience(gone);
  first.reviewer.close();

  const second = await setup({ store, starterAudiences: STARTER_AUDIENCES });
  const ids = second.reviewer.getState().settings.audiences.map((a) => a.id);
  assert.equal(ids.length, 2);
  assert.ok(!ids.includes(gone), 'not put back');
  second.reviewer.close();
});

test('a request Google turns away for load (503) is retried, and answers the second time', async () => {
  const { retrying } = await import('../src/core/gemini.mjs');
  const answers = [new Response('{}', { status: 503 }), new Response('{"ok":true}', { status: 200 })];
  const waits = [];
  const fetch = retrying(async () => answers.shift(), [10, 20], async (ms) => { waits.push(ms); });
  const response = await fetch('https://example.invalid', {});
  assert.equal(response.status, 200);
  assert.deepEqual(waits, [10]);
  const refused = retrying(async () => new Response('{}', { status: 400 }), [10], async (ms) => waits.push(ms));
  assert.equal((await refused('x', {})).status, 400, 'a 400 is not retried');
  assert.deepEqual(waits, [10]);
});
