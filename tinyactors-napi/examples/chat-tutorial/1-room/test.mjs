import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`1-room, ${datamodel}: a post reaches every member`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases(['ada sees ada: hi', 'bob sees ada: hi']));
  });
}
