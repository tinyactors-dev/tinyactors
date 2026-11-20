import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`3-workspace, ${datamodel}: the workspace makes rooms, and a join of a missing room fails`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases([
      'create general: room.created',
      'create general: room.refused (exists)',
      'create Bad Name: room.refused (invalid name)',
      'rooms: general',
      'ada joined general',
      'bob gets an error: no such room',
      'bob joined general',
      'bob sees ada in general: hi',
      'ada sees ada in general: hi',
    ]));
  });
}
