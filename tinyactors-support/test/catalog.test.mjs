// The catalog: every issue in full in one SQLite database, beside the
// ledger it is a projection of. Each change written in one transaction
// before its answer; reads and full text search that wake no actor; the
// decision model's candidates from the search; and recovery both ways: the
// actors rebuilt from the catalog (a lost snapshot), the catalog rebuilt
// from the ledger, and the one-time migration from the ledger of before.

import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { describe, it } from 'node:test';
import { Catalog } from '../lib/processors/catalog.mjs';
import { oldIssues } from '../lib/migrate.mjs';
import { stack } from './stack.mjs';

const napi = 'tinyactors-dev/tinyactors-napi';
const report = (component, title, body = title, more = {}) =>
  ({ repo: napi, component, kind: 'tooling', minutes: 5, commit: 'c2', title, body, ...more });
const id = (n) => `tinyactors:tinyactors-napi:${n}`;
const reads = (s) => s.tool(null, 'known_issues', { state: 'all' }).then((r) => r.issues);

describe('the catalog', () => {
  it('writes every change, ledger and rows in one transaction, before the answer', async () => {
    const s = await stack();
    const ann = (await s.signIn('code-ann')).access_token, bob = (await s.signIn('code-bob')).access_token;
    const filed = await s.tool(ann, 'report_issue', report('lib', 'the convenience layer drops an error', 'a handler error vanishes', { minutes: 12 }));
    assert.equal(filed.answer, 'filed');
    let row = s.catalog.get('tinyactors-napi', filed.n);
    assert.deepEqual([row.title, row.body, row.state, row.count, row.minutes], ['the convenience layer drops an error', 'a handler error vanishes', 'new', 1, 12]);
    assert.equal(s.catalog.records.at(-1).kind, 'filed');
    const counted = await s.tool(bob, 'report_issue', { ...report('lib', 'x'), id: filed.id, minutes: 3 });
    assert.equal(counted.answer, 'counted');
    await s.tool(bob, 'attach_workaround', { id: filed.id, text: 'catch it yourself' });
    await s.tool(bob, 'triage', { id: filed.id, kind: 'api' });
    row = s.catalog.get('tinyactors-napi', filed.n);
    assert.deepEqual([row.count, row.minutes, row.kind, row.state, row.workaround.text, row.occurrences.length],
                     [2, 15, 'api', 'triaged', 'catch it yourself', 2]);
    // the next reporter is answered with the workaround the catalog holds
    assert.equal((await s.tool(ann, 'report_issue', { ...report('lib', 'x'), id: filed.id })).workaround.text, 'catch it yourself');
    // a change the catalog cannot apply leaves no ledger row: one transaction
    const before = s.catalog.records.length;
    assert.throws(() => s.catalog.append('filed', id(99), { title: { not: 'text' } }));
    assert.equal(s.catalog.records.length, before);
    assert.equal(s.catalog.get('tinyactors-napi', 99), null);
    await s.support.close();
  });

  it('searches titles, reports and workarounds with full text, and reads wake no actor', async () => {
    const s = await stack();
    const ann = (await s.signIn('code-ann')).access_token;
    const leak = await s.tool(ann, 'report_issue', report('test', 'the node tests leak hosts', 'each run leaves a host open until the process exits'));
    await s.tool(ann, 'report_issue', report('docs', 'the guide misnames a flag', 'the services guide says --wake'));
    let asked = 0;
    const request = s.support.request.bind(s.support);
    s.support.request = (...args) => { asked++; return request(...args); };
    const found = await s.tool(null, 'known_issues', { query: 'leaking host' });   // stemmed: leak, hosts
    assert.deepEqual(found.issues.map((i) => i.id), [leak.id]);
    assert.equal(found.issues[0].repo, napi);
    assert.deepEqual((await s.tool(null, 'known_issues', { query: 'process exits' })).issues.map((i) => i.id), [leak.id], 'the body is searched');
    assert.deepEqual((await s.tool(null, 'known_issues', { query: '"OR" NEAR(*) ^' })).issues, [], 'no operator from the caller');
    assert.equal((await s.tool(null, 'get_issue', { id: leak.id })).title, 'the node tests leak hosts');
    assert.equal((await s.tool(null, 'get_issue', { id: id(77) })).reason, `no issue #77 in ${napi}`);
    assert.equal(asked, 0, 'no read asked an actor');
    await s.support.close();
  });

  it('gives the decision model the candidates the full text search finds, the report\'s component first', async () => {
    const s = await stack();
    const ann = (await s.signIn('code-ann')).access_token;
    const lib = await s.tool(ann, 'report_issue', report('lib', 'bun cannot load the addon', 'dlopen fails on alpine musl'));
    const src = await s.tool(ann, 'report_issue', report('src', 'the addon segfaults on musl', 'alpine musl crashes in dlopen'));
    await s.tool(ann, 'report_issue', report('docs', 'a typo in the README', 'unrelated words entirely'));
    s.jev.requests.length = 0;
    await s.tool(ann, 'report_issue', { ...report('src', 'musl alpine dlopen trouble', 'dlopen on alpine musl'), kind: undefined });
    const asked = s.jev.requests.at(-1).state.issues.map((i) => i.id);
    assert.deepEqual(asked.slice(0, 2), [`p${src.n}`, `p${lib.n}`], 'both by their words, the same component first');
    assert.equal(asked.length, 2, 'an issue sharing no word is not a candidate');
    await s.support.close();
  });

  it('rebuilds the actors from the catalog when the snapshot is lost: numbers go on, issues behave as before', async () => {
    let s = await stack();
    const ann = (await s.signIn('code-ann')).access_token, bob = (await s.signIn('code-bob')).access_token;
    const a = await s.tool(ann, 'report_issue', report('lib', 'the first problem of the lib'));
    const b = await s.tool(ann, 'report_issue', report('examples', 'an example needs a build first'));
    const c = await s.tool(ann, 'report_issue', report('tools', 'a spam report'));
    await s.tool(bob, 'triage', { id: a.id });
    await s.tool(bob, 'attach_workaround', { id: b.id, text: 'run mise run build first' });
    await s.tool(bob, 'delete_issue', { id: c.id, reason: 'spam' });
    const before = await reads(s), one = await s.tool(null, 'get_issue', { id: a.id });
    const { github, jev, catalog, blobs, scanner } = s;
    await s.support.close();
    s = await stack({ data: s.data, shared: { github, jev, catalog, blobs, scanner }, rebuild: true });
    await s.advance(1000);
    assert.equal(s.support.restored, false);
    assert.deepEqual(await reads(s), before);
    assert.deepEqual(await s.tool(null, 'get_issue', { id: a.id }), one);
    // the actors are back: logins are not (people sign in again), the issues answer as before
    assert.equal((await s.tool(ann, 'whoami')).error, 'sign in first');
    const ann2 = (await s.signIn('code-ann')).access_token, bob2 = (await s.signIn('code-bob')).access_token;
    const again = await s.tool(ann2, 'report_issue', report('examples', 'an example needs a build first'));
    assert.deepEqual([again.answer, again.id, again.workaround.text], ['counted', b.id, 'run mise run build first']);
    assert.equal((await s.tool(bob2, 'triage', { id: a.id })).answer, 'refused', 'the first issue is triaged already');
    assert.equal((await s.tool(bob2, 'triage', { id: b.id })).answer, 'triaged');
    assert.equal((await s.tool(bob2, 'triage', { id: c.id })).reason, `${c.id} was deleted`);
    const next = await s.tool(ann2, 'report_issue', report('test', 'a new problem after the rebuild'));
    assert.equal(next.id, id(c.n + 1), 'the deleted issue\'s number is not reused');
    await s.support.close();
  });

  it('rebuilds its rows from the ledger', async () => {
    const s = await stack();
    const ann = (await s.signIn('code-ann')).access_token, bob = (await s.signIn('code-bob')).access_token;
    const a = await s.tool(ann, 'report_issue', report('lib', 'the lib forgets a handler'));
    await s.tool(bob, 'report_issue', { ...report('lib', 'x'), id: a.id, minutes: 7 });
    await s.tool(bob, 'attach_workaround', { id: a.id, text: 'keep a reference' });
    await s.tool(bob, 'wontfix', { id: a.id, reason: 'by design' });
    const before = await reads(s), one = s.catalog.get('tinyactors-napi', a.n);
    s.catalog.database.exec('DELETE FROM issues; DELETE FROM reports; DELETE FROM issues_fts;');   // the rows lost
    assert.equal(s.catalog.rebuild(), 1);
    assert.deepEqual(await reads(s), before);
    assert.deepEqual(s.catalog.get('tinyactors-napi', a.n), one);
    assert.deepEqual((await s.tool(null, 'known_issues', { query: 'reference', state: 'all' })).issues.map((i) => i.id), [a.id]);
    await s.support.close();
  });

  it('reads the issue actors\' variables for the migration, hibernated ones woken', async () => {
    const s = await stack({ resident: 2 });
    const ann = (await s.signIn('code-ann')).access_token;
    const filed = [];
    for (const t of ['one problem with lib', 'two problems with src', 'three problems with docs']) {
      filed.push(await s.tool(ann, 'report_issue', report(t.split(' ').at(-1), t, t, { minutes: 4 })));
    }
    const read = await oldIssues(s.support);
    assert.deepEqual(read.map((i) => i.id).sort(), filed.map((f) => f.id).sort());
    assert.ok(read.every((i) => i.title && i.state === 'new' && i.repoID === 1002 && i.count === 1));
    await s.support.close();
  });

  it('migrates the ledger of before once, with what the old actors held, and marks what none held as deleted', () => {
    const dir = mkdtempSync(join(tmpdir(), 'catalog-')), legacy = join(dir, 'ledger.sqlite');
    const old = new DatabaseSync(legacy);
    old.exec('CREATE TABLE ledger (seq INTEGER PRIMARY KEY, at TEXT NOT NULL, kind TEXT NOT NULL, key TEXT NOT NULL, record TEXT NOT NULL)');
    const add = old.prepare('INSERT INTO ledger (at, kind, key, record) VALUES (?, ?, ?, ?)');
    const k = (n) => `tinyactors:tinyactors:${n}`;
    add.run('2026-11-27T10:00:00.000Z', 'filed', k(1), JSON.stringify({ component: 'docs', title: 'a guide is wrong', kind: 'docs',
      body: 'the guide says x', minutes: 10, reporter: { githubID: 7, login: 'ann' } }));
    add.run('2026-11-27T11:00:00.000Z', 'counted', k(1), JSON.stringify({ session: 's', commit: 'c', at: null, reporter: 'bob' }));
    add.run('2026-11-27T12:00:00.000Z', 'decided', 'tinyactors: a guide is wrong', JSON.stringify({ model: 'jev' }));
    add.run('2026-11-27T13:00:00.000Z', 'filed', k(2), JSON.stringify({ component: 'docs', title: 'spam', kind: 'docs', body: '' }));
    add.run('2026-11-27T14:00:00.000Z', 'workaround', k(1), JSON.stringify({ text: 'read y', by: 'bob' }));
    old.close();
    const issues = [{ id: k(1), repoID: '1001', title: 'a guide is wrong', component: 'docs', kind: 'docs', body: 'the guide says x',
      count: 2, minutes: 35, reporters: [7, 8], workaround: { text: 'read y', by: 'bob' }, reason: null, fix: null, state: 'triaged',
      attachments: [], purged: false, filed: '2026-11-27T10:00:00.000Z', updated: '2026-11-27T14:00:00.000Z' }];
    const catalog = new Catalog(join(dir, 'catalog.sqlite'), { now: () => Date.UTC(2026, 10, 28) });
    catalog.projects([{ repoID: '1001', slug: 'tinyactors', name: 'tinyactors-dev/tinyactors' }]);
    assert.equal(catalog.migrate({ legacy, issues }), true);
    const snapshot = () => JSON.stringify([catalog.records, catalog.list({ state: 'all' }), catalog.get('tinyactors', 1), catalog.get('tinyactors', 2)]);
    const first = snapshot();
    assert.equal(catalog.migrate({ legacy, issues }), false, 'once');
    assert.equal(snapshot(), first);
    const one = catalog.get('tinyactors', 1);
    assert.deepEqual([one.repoID, one.state, one.count, one.minutes, one.workaround.text, one.filed, one.updated, one.reporterIDs],
                     ['1001', 'triaged', 2, 35, 'read y', '2026-11-27T10:00:00.000Z', '2026-11-27T14:00:00.000Z', [7, 8]]);
    assert.equal(one.occurrences.length, 2);
    assert.deepEqual(catalog.get('tinyactors', 2), { gone: true, id: k(2) });
    assert.deepEqual(catalog.project('1001').next, 3);
    // and its rows are the ledger's: a replay gives them again
    catalog.rebuild();
    assert.equal(snapshot(), first);
    catalog.close();
  });
});
