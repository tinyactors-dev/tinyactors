import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`4-roster, ${datamodel}: a user who comes online gets the roster, and online users get each change`, async () => {
    assert.deepEqual(phases(await run({ datamodel })), phases([
      '-- ada connects',
      'ada sees the roster: ada active, bob offline, cat offline',
      '-- bob connects',
      'bob sees the roster: ada active, bob active, cat offline',
      'ada sees bob is active',
      '-- 300 seconds pass',
      'bob sees ada is away',
      'ada sees bob is away',
      '-- bob closes the app',
      'ada sees bob is offline',
      'the workspace answers: {"ada":"away","bob":"offline","cat":"offline"}',
    ]));
  });
}
