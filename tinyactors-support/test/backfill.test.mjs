// Issues filed before the charts kept when they were filed and last
// changed get those times from the ledger at startup, once: an issue
// without them gets its first `filed` record's time and its latest
// record's; one with them keeps them; a second start sends nothing.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { BASE, stack } from './stack.mjs';
import { backfillTimestamps } from '../lib/backfill.mjs';
import { MemoryBlobs } from '../lib/processors/blobs.mjs';
import { Ledger } from '../lib/processors/ledger.mjs';
import { FakeGitHub, FakeJev } from '../lib/processors/standins.mjs';

const napi = 'tinyactors-dev/tinyactors-napi';
const fresh = (component, title) => ({ repo: napi, component, kind: 'tooling', minutes: 5, title, body: title });

describe('the times of issues from before they were kept', () => {
  it('come from the ledger once, never overwrite, and a restart sends nothing', async () => {
    // A ledger that answers as before it gave the time: the issues keep none, the ledger does.
    const ledger = new Ledger(), write = ledger.processor, old = { on: true };
    ledger.processor = (event, data) => {
      const answer = write(event, data);
      if (answer && old.on) delete answer.data.at;
      return answer;
    };
    const shared = { github: new FakeGitHub(), jev: new FakeJev(), ledger, blobs: new MemoryBlobs({ secret: 'test', base: BASE }) };
    const s = await stack({ shared });
    const ann = (await s.signIn('code-ann')).access_token;
    const bob = (await s.signIn('code-bob')).access_token;
    const first = await s.tool(ann, 'report_issue', fresh('lib', 'the convenience layer drops a handler\'s error'));
    await s.advance(60_000);
    const second = await s.tool(ann, 'report_issue', fresh('test', 'the node tests leak a host'));
    await s.advance(60_000);
    assert.equal((await s.tool(bob, 'triage', { id: first.id, kind: 'api' })).answer, 'triaged');
    for (const issue of [first, second]) assert.equal((await s.tool(null, 'get_issue', { id: issue.id })).filed, null);

    old.on = false;   // the charts as they are now: a new issue keeps its times
    await s.advance(60_000);
    const third = await s.tool(ann, 'report_issue', fresh('src', 'the addon leaks a reference per payload'));
    const kept = await s.tool(null, 'get_issue', { id: third.id });
    assert.match(kept.filed, /^\d{4}-\d\d-\d\dT/);

    assert.equal(await backfillTimestamps({ support: s.support, ledger }), 2);
    await s.idle();
    const at = (id, kind) => ledger.records.filter((r) => r.key === id && (!kind || r.kind === kind)).map((r) => r.at);
    for (const issue of [first, second]) {
      const read = await s.tool(null, 'get_issue', { id: issue.id });
      assert.equal(read.filed, at(issue.id, 'filed')[0], `${issue.id}: filed is its first record's time`);
      assert.equal(read.updated, at(issue.id).at(-1), `${issue.id}: updated is its latest record's time`);
      const listed = (await s.tool(null, 'known_issues', { state: 'all' })).issues.find((i) => i.id === issue.id);
      assert.deepEqual([listed.filed, listed.updated], [read.filed, read.updated], 'the index has them too');
    }
    assert.notEqual((await s.tool(null, 'get_issue', { id: first.id })).updated,
                    (await s.tool(null, 'get_issue', { id: first.id })).filed, 'the triage came later');
    const third2 = await s.tool(null, 'get_issue', { id: third.id });
    assert.deepEqual([third2.filed, third2.updated], [kept.filed, kept.updated], 'an issue with its times keeps them');
    const records = ledger.records.length;

    assert.equal(await backfillTimestamps({ support: s.support, ledger }), 0, 'a second start sends nothing');
    await s.support.close();
    const again = await stack({ data: s.data, shared });   // a restart, from the snapshot
    assert.equal(again.support.restored, true);
    assert.equal(await backfillTimestamps({ support: again.support, ledger }), 0, 'nor does a restart');
    const restored = await again.tool(null, 'get_issue', { id: first.id });
    assert.equal(restored.filed, at(first.id, 'filed')[0]);
    assert.equal(ledger.records.length, records, 'the backfill wrote no ledger record');
    await again.support.close();
  });
});
