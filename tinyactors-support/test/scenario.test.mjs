// A day, and then a quarter, at the support channel: every behaviour of the
// charts, through the gateway, on a SimulatedClock with the stand-ins. The
// steps share one service and run in order.

import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { stack, sha, zip } from './stack.mjs';

const report = {
  repo: 'tinyactors-dev/tinyactors-napi', component: 'examples', kind: 'tooling',
  title: 'mise run examples fails before mise run build', minutes: 20,
  body: 'cannot find build/tinyactors.node when the examples run before the addon is built',
  session: 'https://claude.ai/code/session_ann', commit: 'a1',
};
const issue1 = 'tinyactors:tinyactors-napi:1';

describe('the support channel', () => {
  let s, ann, bob, cy, dee, annTokens, cyTokens;
  const news = [];   // what ann's answers carried "since her last visit"
  const toolAnn = async (name, args) => { const r = await s.tool(ann, name, args); news.push(...(r.sinceYourLastVisit ?? [])); return r; };
  const attachAnn = async (...rest) => { const r = await s.attach(ann, ...rest); news.push(...(r.slot.sinceYourLastVisit ?? [])); return r; };

  before(async () => { s = await stack(); });
  after(async () => { await s.support.close(); });

  it('boots: the tracker pulls the repositories, each project its components and commits', () => {
    assert.equal(s.github.calls['repos.list'], 1);
    assert.equal(s.github.calls['contents.list'], 3);
    assert.equal(s.github.calls['commits.since'], 3);
  });

  it('answers anyone: skills, docs, known issues; a report needs a login', async () => {
    assert.deepEqual((await s.tool(null, 'list_skills')).skills.map((k) => k.name), ['reporting-tinyactors-issues']);
    assert.deepEqual((await s.tool(null, 'search_docs', { query: 'hibernation store' })).hits.map((h) => h.path), ['docs/hibernation.md']);
    assert.deepEqual((await s.tool(null, 'search_docs', { query: 'migrate python' })).hits, []);
    assert.deepEqual(s.gateway.misses, ['migrate python']);
    assert.deepEqual((await s.tool(null, 'known_issues')).issues, []);
    assert.equal((await s.tool(null, 'report_issue', report)).status, 401);
  });

  it('signs people in; a forged token and a token for no grant are nobody', async () => {
    annTokens = await s.signIn('code-ann');
    cyTokens = await s.signIn('code-cy');
    ann = annTokens.access_token; cy = cyTokens.access_token;
    bob = (await s.signIn('code-bob')).access_token;
    dee = (await s.signIn('code-dee')).access_token;
    assert.equal((await s.tool(ann, 'whoami')).login, 'ann');
    assert.deepEqual((await s.tool(bob, 'whoami')).maintainerOf, ['1001', '1002']);
    assert.equal(await s.gateway.caller(`${ann.split('.')[0]}.forged`), null);
    assert.equal(await s.gateway.caller('ffffffffffff.x'), null);
  });

  it('files one issue for two reports of it at once: the second waits for the spawn and is counted', async () => {
    const callerOf = (token) => s.gateway.caller(token);
    const [first, second] = await Promise.all([
      s.support.request('tracker', 'report', { ...report, caller: await callerOf(ann) }),
      s.support.request('tracker', 'report', { ...report, title: 'Mise: examples FAILS before build, run!', minutes: 15,
                                               session: 'https://claude.ai/code/session_cy', caller: await callerOf(cy) }),
    ]);
    assert.equal(first.event, 'filed');
    assert.equal(first.data.id, issue1);
    assert.equal(second.event, 'counted');
    assert.equal(second.data.count, 2);
  });

  it('refuses what does not fit, naming what would', async () => {
    const repo = await toolAnn('report_issue', { ...report, repo: 'tinyactors-dev/nope' });
    assert.equal(repo.answer, 'refused');
    assert.ok(repo.valid.includes('tinyactors-dev/tinyactors-napi'));
    const component = await toolAnn('report_issue', { ...report, component: 'benches' });
    assert.deepEqual(component.valid, ['docs', 'examples', 'lib', 'src', 'test', 'tools']);
  });

  it('asks the decision model when no issue matches exactly, and decides what its answer means', async () => {
    const paraphrase = await s.tool(cy, 'report_issue', { ...report, kind: undefined, minutes: 5,
      title: 'examples need the addon built first',
      body: 'running the examples fails: cannot find build/tinyactors.node until the addon is built' });
    assert.equal(paraphrase.answer, 'counted');
    assert.equal(paraphrase.matchedBy.model, 'jev');
    const unsure = await s.tool(cy, 'report_issue', { ...report, kind: undefined, minutes: 5,
      title: 'examples are slow until the addon build is cached', body: 'every example run rebuilds the addon from scratch' });
    assert.equal(unsure.answer, 'filed');
    assert.equal(unsure.possibleDuplicateOf.id, issue1);
    const different = await toolAnn('report_issue', { repo: report.repo, component: 'docs', minutes: 3,
      title: 'the README links to a guide that is missing', body: 'docs/guide/stores.md is linked but not in the package' });
    assert.equal(different.answer, 'filed');
    assert.equal(different.possibleDuplicateOf, null);
    assert.equal((await s.tool(null, 'get_issue', { id: different.id })).kind, 'docs');
  });

  it('asks about the issue the first of two new reports made, when the second waited for its decision', async () => {
    const r = { repo: report.repo, component: 'src', minutes: 30, kind: 'api' };
    const [one, two] = await Promise.all([
      s.support.request('tracker', 'report', { ...r, caller: await s.gateway.caller(ann),
        title: 'Host.restore loses the clock offset', body: 'after a restore the snapshot deadlines fire late by the snapshot age' }),
      s.support.request('tracker', 'report', { ...r, caller: await s.gateway.caller(bob),
        title: 'deadlines fire late after a restore', body: 'the restored clock offset is lost, so the snapshot deadlines fire late' }),
    ]);
    assert.equal(one.event, 'filed');
    assert.equal(two.event, 'counted');
    assert.equal(two.data.id, one.data.id);
  });

  it('files a report anyway when the model is down, or never answers', async () => {
    s.jev.unavailable = 1;
    assert.equal((await s.tool(bob, 'report_issue', { ...report, kind: undefined,
      title: 'examples print nothing on failure', body: 'mise run examples exits 1 with no output' })).answer, 'filed');
    s.jev.silent = 1;
    const quiet = s.support.request('tracker', 'report', { ...report, kind: undefined, caller: await s.gateway.caller(bob),
      title: 'example tests time out on bun', body: 'bun test examples hangs for a minute' }, 60_000);
    await s.idle();
    await s.advance(5000);
    assert.equal((await quiet).event, 'filed');
  });

  it('gives the next reporter a workaround at once, and the earlier ones on their next call', async () => {
    assert.equal((await toolAnn('attach_workaround', { id: issue1, text: 'run `mise run build` in tinyactors-napi first' })).answer, 'noted');
    const third = await s.tool(bob, 'report_issue', { ...report, minutes: 2 });
    assert.equal(third.workaround.text, 'run `mise run build` in tinyactors-napi first');
    const told = (await s.tool(cy, 'whoami')).sinceYourLastVisit;
    assert.ok(told.some((u) => u.state === 'workaround' && u.id === issue1));
  });

  it('lets a maintainer of the repository triage, and nobody else', async () => {
    assert.equal((await toolAnn('triage', { id: issue1, kind: 'tooling' })).answer, 'refused');
    assert.equal((await s.tool(bob, 'triage', { id: issue1, kind: 'tooling' })).answer, 'triaged');
    const known = await s.tool(null, 'known_issues', { repo: report.repo });
    assert.equal(known.issues[0].id, issue1);                  // the costliest first
    assert.equal(known.issues[0].state, 'triaged');
    assert.equal(known.issues[0].repo, 'tinyactors-dev/tinyactors-napi');
  });

  it('takes attachments out of band: checked, redacted, refused or rejected, readable by reporters and maintainers', async () => {
    const log = 'npm warn\nAuthorization: Bearer ghp_0123456789abcdefghijABCDEFGHIJ0123\n' +
      'token=ghp_ZYXWVUTSRQPONMLKJIHGFEDCBA9876543210\nerror: cannot find build/tinyactors.node\n';
    const logged = await attachAnn(issue1, 'mise-examples.log', 'text/plain', Buffer.from(log));
    assert.equal(logged.put.status, 201);
    assert.match(logged.slot.uploadURL, /^https:\/\/support\.test\/uploads\/1002\.1\.1$/);
    const png = Buffer.concat([Buffer.from('89504e470d0a1a0a0000000d49484452', 'hex'), Buffer.alloc(64, 7)]);
    assert.equal((await attachAnn(issue1, 'error dialog.png', 'image/png', png)).put.status, 201);
    const repro = zip([['transcript.jsonl', '{"jsonrpc":"2.0","id":1,"method":"run.turn"}\n'], ['charts/order.scxml', '<scxml/>']]);
    assert.equal((await attachAnn(issue1, 'repro.zip', 'application/zip', repro)).put.status, 201);
    const snapshot = Buffer.from(s.support.host.snapshot());
    assert.equal((await attachAnn(issue1, 'host.snapshot', 'application/x-tinyactors-snapshot', snapshot)).put.status, 201);

    const mine = await toolAnn('get_issue', { id: issue1 });
    assert.deepEqual(mine.attachments.map((a) => a.scan.verdict), ['redacted', 'ok', 'ok', 'ok']);
    assert.match(mine.attachments[2].scan.summary, /2 entries: transcript\.jsonl/);
    assert.match(mine.attachments[3].scan.summary, /Format|tinyactors snapshot/);
    assert.equal((await s.tool(null, 'get_issue', { id: issue1 })).attachments, 4);   // anyone: how many, no more
    const link = await s.tool(bob, 'get_attachment', { id: issue1, upload: mine.attachments[0].upload });
    const url = new URL(link.url);
    const key = url.pathname.slice('/files/'.length).split('/').map(decodeURIComponent).join('/');
    assert.ok(s.blobs.verify(key, url.searchParams.get('expires'), url.searchParams.get('signature')));
    assert.ok(!s.blobs.verify(key, url.searchParams.get('expires'), 'forged'));
    const stored = s.blobs.get(key).toString();
    assert.ok(!stored.includes('ghp_'));
    assert.equal(stored.split('[REDACTED]').length, 3);
    assert.ok(!s.blobs.keys().some((k) => k.endsWith('mise-examples.log')), 'the original log is gone');

    assert.match((await attachAnn(issue1, 'diagram.svg', 'image/svg+xml', Buffer.from('<svg/>'))).slot.reason, /not a type/);
    assert.match((await toolAnn('attach_file', { id: issue1, name: 'run.webm', type: 'video/webm', size: 150 * 1024 * 1024, sha256: sha('x') })).reason, /larger than 100 MB/);
    await attachAnn(issue1, 'shot.png', 'image/png', Buffer.from('not a png'));
    await attachAnn(issue1, 'b.log', 'text/plain', Buffer.from('ok'), { sha256: sha('other') });
    assert.equal((await attachAnn(issue1, 'c.log', 'text/plain', Buffer.from('longer than said'), { size: 4 })).put.status, 413);
    await attachAnn(issue1, 'evil.zip', 'application/zip', zip([['../../.bashrc', 'x']]));
    await attachAnn(issue1, 'bomb.zip', 'application/zip', zip([['all.log', 'aaaaaaaaaa']], { claim: 10_000_000 }));
    s.scanner.unavailable = 1;
    await attachAnn(issue1, 'd.log', 'text/plain', Buffer.from('fine'));
    await toolAnn('whoami');
    const rejected = news.filter((u) => u.state === 'attachment rejected').map((u) => `${u.file}: ${u.reason}`);
    assert.deepEqual(rejected, [
      'shot.png: declared image/png, but the file is text/plain', 'b.log: the sha256 is not the one declared',
      'c.log: more bytes than declared', 'evil.zip: an entry leaves the archive: ../../.bashrc',
      'bomb.zip: all.log expands 1000000 times: a zip bomb?', 'd.log: the file could not be checked']);
    assert.equal((await toolAnn('get_issue', { id: issue1 })).attachments.length, 4);

    assert.equal((await s.tool(dee, 'attach_file', { id: issue1, name: 'e.log', type: 'text/plain', size: 2, sha256: sha('hi') })).answer, 'refused');
    assert.equal((await s.tool(dee, 'get_attachment', { id: issue1, upload: mine.attachments[0].upload })).answer, 'refused');
    const slot = await toolAnn('attach_file', { id: issue1, name: 'f.log', type: 'text/plain', size: 2, sha256: sha('hi') });
    assert.equal((await s.gateway.upload({ token: bob, uploadID: slot.uploadID, chunks: [Buffer.from('hi')] })).status, 403);
    await s.advance(900_000);
    assert.equal((await s.gateway.upload({ token: ann, uploadID: slot.uploadID, chunks: [Buffer.from('hi')] })).status, 404);
    const outcomes = s.ended.filter((d) => d?.upload).map((d) => d.outcome).sort();
    assert.deepEqual(outcomes, ['attached', 'attached', 'attached', 'attached', 'expired', 'refused', 'refused',
                                'rejected', 'rejected', 'rejected', 'rejected', 'rejected', 'rejected']);
  });

  it('marks it fixed when a monorepo commit\'s trailer names it, routed to its project; an older commit is told to update, a newer one regresses it', async () => {
    s.github.commit(1001, 'm2', 'fix(napi): examples build what they need\n\nFixes-issue: tinyactors:tinyactors-napi:1\n' +
      'Fixes-issue: tinyactors:tinyactors-napi:99');
    await s.advance(600_000);
    assert.equal((await s.tool(null, 'get_issue', { id: issue1 })).fix.sha, 'm2');
    assert.equal((await toolAnn('whoami')).sinceYourLastVisit.at(-1).state, 'fixed');
    const older = await s.tool(cy, 'report_issue', { ...report, commit: 'a1' });
    assert.equal(older.answer, 'fixed');
    assert.match(older.advice, /predates the fix/);
    s.github.commit(1001, 'm3', 'refactor(napi): the example runner');
    assert.equal((await s.tool(cy, 'report_issue', { ...report, commit: 'm3' })).answer, 'regressed');
  });

  it('closes a fixed issue after a fortnight with no report, and deletes its attachments\' bytes 90 days later', async () => {
    s.github.commit(1001, 'm4', 'fix(napi): build before the runner\n\nFixes-issue: tinyactors:tinyactors-napi:1');
    await s.advance(600_000);
    await s.advance(14 * 86_400_000);
    assert.ok(!(await s.tool(null, 'known_issues', { repo: report.repo })).issues.some((i) => i.id === issue1));
    const of1 = () => s.blobs.keys().filter((k) => k.startsWith('issues/tinyactors-napi/1/')).length;
    assert.equal(of1(), 4);
    await s.advance(90 * 86_400_000);
    assert.equal(of1(), 0);
  });

  it('expires access tokens after an hour; a refresh rotates them, and an old refresh token used again revokes the grant', async () => {
    assert.equal(await s.gateway.caller(ann), null);
    ann = (await s.gateway.refresh({ refreshToken: annTokens.refresh_token })).access_token;
    assert.equal((await s.tool(ann, 'whoami')).login, 'ann');
    const cyNew = await s.gateway.refresh({ refreshToken: cyTokens.refresh_token });
    assert.equal(cyNew.status, 200);
    assert.equal((await s.gateway.refresh({ refreshToken: cyTokens.refresh_token })).status, 400);
    assert.equal(await s.gateway.caller(cyNew.access_token), null);
    bob = (await s.signIn('code-bob')).access_token;
  });

  it('rides out GitHub being down; an archived repository refuses reports; a renamed one keeps its identifiers', async () => {
    s.github.faults['repos.list'] = 1;
    s.github.repos[1].name = 'tinyactors-dev/tinyactors-node';
    s.github.repos[2].archived = true;
    await s.advance(600_000);   // the pull: 503, Retry-After 120
    await s.advance(120_000);   // the retry
    const archived = await toolAnn('report_issue', { repo: 'tinyactors-dev/scxmljs', component: 'site', title: 'the playground is slow', minutes: 3 });
    assert.equal(archived.reason, 'tinyactors-dev/scxmljs is archived');
    const two = await s.tool(null, 'get_issue', { id: 'tinyactors:tinyactors-napi:2' });
    assert.equal(two.repo, 'tinyactors-dev/tinyactors-node');
    const oldName = await toolAnn('report_issue', { ...report, title: 'the README of the examples is stale', component: 'docs' });
    assert.match(oldName.id, /^tinyactors:tinyactors-napi:/);
    assert.equal((await s.tool(null, 'get_issue', { id: 'napi#2' })).status, 400);
  });

  it('comes back from its snapshot and store after a restart, tokens and all', async () => {
    const { github, jev, ledger, blobs, scanner } = s;
    await s.support.close();
    s = await stack({ data: s.data, shared: { github, jev, ledger, blobs, scanner } });
    assert.ok(s.support.restored);
    const filed = await s.tool(ann, 'report_issue', { ...report, title: 'the examples task is slow', minutes: 4 });
    assert.equal(filed.answer, 'filed');
    assert.equal((await s.tool(null, 'get_issue', { id: issue1 })).attachments, 4);
    assert.ok(ledger.records.some((r) => r.kind === 'filed' && r.key === filed.id));
    assert.deepEqual(s.faults, []);
  });
});
