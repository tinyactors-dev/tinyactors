// The tools as a client sees them: every answer kind of every tool checked
// against its output schema and its text (test/stack.mjs's checked(), which
// every s.tool() call runs), the maintainer's actions answered in every
// state, and deleting an issue, with the reports on their way to it.

import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { checked, seen, sha, stack } from './stack.mjs';
import { TOOLS } from '../lib/tools.mjs';

const napi = 'tinyactors-dev/tinyactors-napi';
const report = { repo: napi, component: 'examples', kind: 'tooling', minutes: 20, commit: 'a1',
  title: 'mise run examples fails before mise run build',
  body: 'cannot find build/tinyactors.node when the examples run before the addon is built' };
const id = (n) => `tinyactors:tinyactors-napi:${n}`;
// a report about something else: its own words, so nothing matches it but itself
const fresh = (component, title) => ({ repo: napi, component, kind: 'tooling', minutes: 5, title, body: title });

describe('the tools: typed results, short text', () => {
  let s, ann, bob, cy;
  before(async () => {
    s = await stack();
    ann = (await s.signIn('code-ann')).access_token;
    bob = (await s.signIn('code-bob')).access_token;   // maintains tinyactors and tinyactors-napi
    cy = (await s.signIn('code-cy')).access_token;
  });
  after(async () => { await s.support.close(); });

  it('reads for anyone: skills, docs, projects, issues; refusals and errors are typed too', async () => {
    const skills = await s.tool(null, 'list_skills');
    assert.ok(skills.skills.length > 0);
    assert.equal((await s.tool(null, 'get_skill', { name: skills.skills[0].name })).name, skills.skills[0].name);
    assert.equal((await s.tool(null, 'get_skill', { name: 'nope' })).answer, 'refused');
    const using = await s.tool(null, 'get_skill', { name: 'using-tinyactors' });
    assert.equal(using.path, 'SKILL.md');
    assert.ok(using.files.includes('references/designing-actors.md'), 'get_skill lists the skill\'s files');
    for (const path of using.files) {
      const file = await s.tool(null, 'get_skill', { name: 'using-tinyactors', path });
      assert.equal(file.path, path);
      assert.match(file.text, /^# /);
    }
    const missing = await s.tool(null, 'get_skill', { name: 'using-tinyactors', path: 'references/nope.md' });
    assert.equal(missing.answer, 'refused');
    assert.ok(missing.valid.includes('references/pitfalls.md'));
    const hits = (await s.tool(null, 'search_docs', { query: 'actor' })).hits;
    if (hits.length) assert.equal((await s.tool(null, 'get_doc', { path: hits[0].path })).path, hits[0].path);
    assert.equal((await s.tool(null, 'get_doc', { path: 'docs/nope.md' })).answer, 'refused');
    const projects = (await s.tool(null, 'list_projects')).projects;
    const napiProject = projects.find((p) => p.repo === napi);
    assert.deepEqual(napiProject, { repoID: '1002', repo: napi, slug: 'tinyactors-napi', archived: false,
                                    components: napiProject.components });
    assert.ok(napiProject.components.includes('examples'));
    assert.deepEqual((await s.tool(null, 'known_issues', { state: 'all' })).issues, []);
    assert.equal((await s.tool(null, 'get_issue', { id: id(1) })).answer, 'refused');
    assert.match((await s.tool(null, 'get_issue', { id: 'napi#1' })).error, /not an issue identifier/);
    assert.match((await s.tool(null, 'no_such_tool')).error ?? '', /no tool/);
  });

  it('answers a report in every way it can: filed, counted, refused, failed, fixed, regressed', async () => {
    assert.equal((await s.tool(ann, 'report_issue', report)).answer, 'filed');
    const counted = await s.tool(cy, 'report_issue', report);
    assert.equal(counted.answer, 'counted');
    assert.match(checked('report_issue', counted), /^Counted on tinyactors:tinyactors-napi:1 \(2 reports\)/);
    const refused = await s.tool(ann, 'report_issue', { ...report, component: 'benches' });
    assert.match(checked('report_issue', refused), /^Refused: unknown component benches \(valid: .*examples/);
    s.ledger.failing = 1;   // the ledger refuses the filing of the next new issue
    const failed = await s.tool(ann, 'report_issue', { repo: napi, component: 'docs', kind: 'docs', title: 'quantum widgets render backwards',
                                                         body: 'zzz' });
    assert.equal(failed.answer, 'failed');
    s.github.commit(1001, 'm2', `fix(napi): examples build what they need\n\nFixes-issue: ${id(1)}`);
    await s.advance(600_000);
    assert.equal((await s.tool(cy, 'report_issue', { ...report, commit: 'a1' })).answer, 'fixed');
    s.github.commit(1001, 'm3', 'refactor(napi): the example runner');
    assert.equal((await s.tool(cy, 'report_issue', { ...report, commit: 'm3' })).answer, 'regressed');
  });

  it('answers a maintainer\'s action in every state, at once: done, or refused with why', async () => {
    const filed = await s.tool(ann, 'report_issue', fresh('src', 'Host.restore loses the clock offset'));
    const n = filed.id;
    assert.equal((await s.tool(ann, 'triage', { id: n, kind: 'api' })).answer, 'refused');   // not a maintainer
    assert.equal((await s.tool(bob, 'triage', { id: n, kind: 'api' })).answer, 'triaged');
    const started = Date.now();
    const again = await s.tool(bob, 'triage', { id: n });
    assert.equal(again.answer, 'refused');
    assert.match(again.reason, /is triaged: only a new issue is triaged/);
    assert.match((await s.tool(bob, 'close', { id: n })).reason, /only a fixed issue is closed/);
    assert.ok(Date.now() - started < 5000, 'no 10-second wait for an answer');
    assert.equal((await s.tool(bob, 'wontfix', { id: n, reason: 'works as designed' })).answer, 'wontfix');
    assert.match((await s.tool(bob, 'wontfix', { id: n, reason: 'again' })).reason, /only an open issue is declined/);
    assert.equal((await s.tool(ann, 'attach_workaround', { id: n, text: 'restore, then advance the clock' })).answer, 'noted');
    assert.equal((await s.tool(null, 'get_issue', { id: n })).state, 'wontfix');
    assert.equal((await s.tool(null, 'known_issues', { state: 'wontfix' })).issues[0].id, n);
    assert.ok(!(await s.tool(null, 'known_issues')).issues.some((i) => i.id === n), 'open is the default');
    assert.ok((await s.tool(ann, 'whoami')).login === 'ann');
  });

  it('deletes an issue for a maintainer: gone from every list and read, its files and its actor gone, its number kept, the ledger told', async () => {
    const filed = await s.tool(ann, 'report_issue', fresh('tools', 'buy cheap watches here'));
    const spam = filed.id;
    const file = Buffer.from('spam spam spam\n');
    const { put } = await s.attach(ann, spam, 'spam.log', 'text/plain', file);
    assert.equal(put.status, 201);
    const keys = () => [...s.blobs.objects.keys()].filter((k) => k.includes(`/${spam.split(':')[2]}/`));
    assert.equal(keys().length, 1);
    const before = s.ledger.records.length;
    assert.equal((await s.tool(ann, 'delete_issue', { id: spam })).answer, 'refused');   // not a maintainer
    const deleted = await s.tool(bob, 'delete_issue', { id: spam, reason: 'spam' });
    assert.equal(deleted.answer, 'deleted');
    assert.equal(checked('delete_issue', deleted), `Deleted ${spam}.`);
    assert.equal(keys().length, 0, 'its files are deleted at once');
    assert.ok(s.ended.some((d) => d?.deleted === spam), 'its actor ended');
    const record = s.ledger.records.at(-1);
    assert.deepEqual([record.kind, record.key, record.record.by, record.record.reason, record.record.title],
                     ['deleted', spam, 'bob', 'spam', 'buy cheap watches here']);
    assert.ok(s.ledger.records.length > before && s.ledger.records.some((r) => r.key === spam && r.kind === 'filed'),
              'the earlier records stay');
    assert.match((await s.tool(null, 'get_issue', { id: spam })).reason, /was deleted/);
    assert.ok(!(await s.tool(null, 'known_issues', { state: 'all' })).issues.some((i) => i.id === spam));
    assert.match((await s.tool(cy, 'report_issue', { ...fresh('tools', 'buy cheap watches here'), id: spam })).reason, /was deleted/);
    assert.match((await s.tool(bob, 'delete_issue', { id: spam })).reason, /was deleted/);
    const anew = await s.tool(cy, 'report_issue', fresh('tools', 'buy cheap watches here'));
    assert.equal(anew.answer, 'filed');
    assert.ok(Number(anew.id.split(':')[2]) > Number(spam.split(':')[2]), 'its number is not reused');
    assert.ok(!((await s.tool(ann, 'whoami')).sinceYourLastVisit ?? []).some((u) => u.id === spam), 'its reporters are not told');
  });

  it('does not lose a report on its way to an issue being deleted', async () => {
    const callerOf = (token) => s.gateway.caller(token, true);
    const [bobCaller, cyCaller] = [await callerOf(bob), await callerOf(cy)];
    // a delete while the issue's spawn is in flight is refused, and the report that made it is filed
    const next = Number((await s.tool(null, 'known_issues', { state: 'all' })).issues.map((i) => Number(i.n))
      .reduce((a, b) => Math.max(a, b), 0)) + 1;   // the newest issue's number, plus one (deleted ones are older)
    const [filed, early] = await Promise.all([
      s.support.request('tracker', 'report', { ...fresh('lib', 'lib exports nothing for bun'), caller: cyCaller }),
      s.support.request('tracker', 'issue.delete', { repo: napi, n: next, caller: bobCaller }),
    ]);
    assert.equal(filed.event, 'filed');
    assert.equal(filed.data.id, id(next));
    assert.equal(early.event, 'refused');
    assert.match(early.data.reason, /still being filed/);
    // a report the model matches to an issue deleted before its answer comes is filed as new, not lost
    const target = filed.data.id;
    const [paraphrase, gone] = await Promise.all([
      s.support.request('tracker', 'report', { ...fresh('lib', 'bun finds no exports in lib'),
                                               body: 'lib exports nothing for bun', caller: cyCaller }),
      s.support.request('tracker', 'issue.delete', { repo: napi, n: next, caller: bobCaller }),
    ]);
    assert.equal(gone.event, 'deleted');
    assert.equal(paraphrase.event, 'filed');
    assert.notEqual(paraphrase.data.id, target);
    // a delete the ledger refuses is failed, and the issue is back where it was
    const kept = await s.tool(ann, 'report_issue', fresh('test', 'the node tests leak a host'));
    s.ledger.failing = 1;
    assert.equal((await s.tool(bob, 'delete_issue', { id: kept.id })).answer, 'failed');
    assert.equal((await s.tool(null, 'get_issue', { id: kept.id })).state, 'new');
    assert.ok((await s.tool(null, 'known_issues')).issues.some((i) => i.id === kept.id));
    assert.equal((await s.tool(cy, 'report_issue', fresh('test', 'the node tests leak a host'))).answer, 'counted');
  });

  it('gives each issue when it was filed and when it last changed, on the host\'s clock', async () => {
    const filed = await s.tool(ann, 'report_issue', fresh('lib', 'the convenience layer drops a handler\'s error'));
    const first = (await s.tool(null, 'get_issue', { id: filed.id }));
    assert.match(first.filed, /^\d{4}-\d\d-\d\dT/);
    assert.equal(first.updated, first.filed);
    await s.advance(600_000);   // ten minutes: an access token lasts an hour
    assert.equal((await s.tool(bob, 'triage', { id: filed.id, kind: 'api' })).answer, 'triaged');
    const later = await s.tool(null, 'get_issue', { id: filed.id });
    assert.equal(later.filed, first.filed, 'filed stays');
    assert.equal(Date.parse(later.updated) - Date.parse(first.filed), 600_000, 'updated is the triage\'s time');
    const listed = (await s.tool(null, 'known_issues')).issues.find((i) => i.id === filed.id);
    assert.deepEqual([listed.filed, listed.updated], [later.filed, later.updated], 'the index has them too');
  });

  it('reached every tool, and the answer kinds the rules name', () => {
    assert.deepEqual(Object.keys(TOOLS).filter((name) => !seen[name] && !['attach_file', 'get_attachment'].includes(name)), []);
    for (const kind of ['filed', 'counted', 'fixed', 'regressed', 'refused', 'failed'])
      assert.ok(seen.report_issue.has(kind), `report_issue answered ${kind}`);
    assert.ok(Object.values(seen).some((kinds) => kinds.has('error')), 'an error');
    assert.ok(sha);
  });
});
