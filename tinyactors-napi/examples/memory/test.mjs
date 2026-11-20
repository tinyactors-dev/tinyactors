// The measurements run, and the core's share is what `mise run layout` in
// tinyactors-core gives: a message 106 B (57 B without tracing), a delayed
// send 151 B (102 B). Run by `mise run examples memory`.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

for (const datamodel of ['native', 'quickjs']) {
  for (const tracing of [true, false]) {
    test(`${datamodel}, tracing ${tracing ? 'on' : 'off'}: each thing costs memory, and the core's share is its records`, () => {
      const results = run({ count: 5000, datamodel, tracing });
      assert.equal(results.message.waiting.region, tracing ? 106 : 57);
      assert.equal(results['delayed send'].pending.region, tracing ? 151 : 102);
      assert.ok(results.actor.started.region > 81, `an actor's region: ${results.actor.started.region} B`);
      for (const row of [results.actor.started, results.actor.handled, results.message.waiting, results['delayed send'].pending]) {
        assert.ok(row.resident > 0, JSON.stringify(row));
      }
    });
  }
}
