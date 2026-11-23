import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`7-reconnect, ${datamodel}: a lost connection is not offline for 30 seconds, the messages wait for the user, and the roster comes whole on return`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases([
      '-- bob enters a tunnel, cy signs out, ada posts',
      'ada sees cy is offline',
      'ada sees ada in general: where are you?',
      '-- 10 seconds later bob is back',
      'bob sees ada in general: where are you?',
      'bob sees the roster: ada active, bob active, cy offline',
      '-- bob loses the connection for 40 seconds',
      'ada sees bob is offline',
      'ada sees ada in general: gone?',
    ]));
  });
}
