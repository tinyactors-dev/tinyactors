import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`6-typing, ${datamodel}: typing is told at most every 3 seconds, and its indicator ends with a post, after 5 seconds, or when the viewer disconnects`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases([
      '-- ada types three keys',
      'bob sees ada typing in general',
      'cat sees ada typing in general',
      '-- 4 seconds pass, ada types and posts',
      'ada sees ada in general: lunch?',
      'bob sees ada stop typing in general',
      'bob sees ada in general: lunch?',
      'cat sees ada stop typing in general',
      'cat sees ada in general: lunch?',
      '-- bob types, then stops for 6 seconds',
      'ada sees bob typing in general',
      'cat sees bob typing in general',
      'ada sees bob stop typing in general',
      'cat sees bob stop typing in general',
      '-- bob types again, then cat disconnects',
      'ada sees bob typing in general',
      'cat sees bob typing in general',
      'cat sees bob stop typing in general',
    ]));
  });
}
