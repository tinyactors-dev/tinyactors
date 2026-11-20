import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`2-presence, ${datamodel}: users turn away when idle, and offline users see nothing`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases([
      'ada is offline', 'bob is offline', 'ada is active', 'bob is active',
      '-- 300 seconds pass',
      'ada is away', 'bob is away',
      '-- bob types, ada closes the app',
      'bob is active', 'ada is offline', 'bob sees bob: anyone?',
    ]));
  });
}
