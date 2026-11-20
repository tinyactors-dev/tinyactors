import assert from 'node:assert/strict';
import { test } from 'node:test';
import { run } from './main.mjs';

for (const datamodel of ['native', 'quickjs']) {
  test(`5-hibernation, ${datamodel}: 1000 users, at most 50 in memory, every message and update delivered`, async () => {
    const lines = await run({ datamodel });
    assert.ok(lines.includes('100 messages reached the screens'), lines.join('\n'));
    // 100 users connect one after another (0 + 1 + ... + 99 updates), then each turns away (100 * 99)
    assert.ok(lines.includes('14850 roster updates reached the screens'), lines.join('\n'));
    assert.ok(lines.includes('at most 50 actors were in memory at rest'), lines.join('\n'));
  });
}
