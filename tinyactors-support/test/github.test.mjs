// The GitHub App's client against a recorded fetch: permissions list the
// installation's repositories first when it has not listed them, a failed
// request is logged with its status and repository, and when every one
// failed the answer is github.unavailable, so the user keeps its roles.

import assert from 'node:assert/strict';
import { generateKeyPairSync } from 'node:crypto';
import { describe, it } from 'node:test';
import { GitHubApp } from '../lib/processors/github.mjs';

const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
const repos = [{ id: 1, full_name: 'tinyactors-dev/tinyactors', archived: false },
               { id: 7, full_name: 'tinyactors-dev/sxml', archived: false }];
const json = (status, body, headers = {}) => new Response(JSON.stringify(body), { status, headers });

function app(answer) {
  const lines = [], asked = [];
  const fetch = async (url) => {
    const path = new URL(url).pathname;
    asked.push(path);
    if (path === '/app/installations') return json(200, [{ id: 9, account: { login: 'tinyactors-dev' } }]);
    if (path === '/app/installations/9/access_tokens') return json(201, { token: 't', expires_at: new Date(Date.now() + 3600e3).toISOString() });
    if (path === '/installation/repositories') return answer.repositories ?? json(200, { total_count: repos.length, repositories: repos });
    const permission = /^\/repos\/(.+)\/collaborators\/[^/]+\/permission$/.exec(path);
    if (permission) return answer[permission[1]] ?? json(200, { permission: 'read', role_name: 'read' });
    return json(404, {});
  };
  const github = new GitHubApp({ appID: 1, clientID: 'c', clientSecret: 's', privateKey, org: 'tinyactors-dev',
                                 base: 'https://support.test', fetch, log: (line) => lines.push(line) });
  return { github, lines, asked };
}

describe('the GitHub App: permissions', () => {
  it('lists the installation\'s repositories first when it has none yet, and logs each role', async () => {
    const { github, lines, asked } = app({ 'tinyactors-dev/sxml': json(200, { permission: 'admin', role_name: 'admin' }) });
    const answer = await github.processor('permissions', { login: 'dhamidi' });
    assert.deepEqual(answer, { event: 'permissions', data: { maintainerOf: [7] } });
    assert.ok(asked.includes('/installation/repositories'));
    assert.match(lines.at(-1), /^permissions of dhamidi: maintains tinyactors-dev\/sxml of 2 repositories \(tinyactors-dev\/sxml admin, tinyactors-dev\/tinyactors read\)$/);
  });

  it('logs a failed request with its status and repository, and answers with the rest', async () => {
    const { github, lines } = app({ 'tinyactors-dev/sxml': json(403, { message: 'Resource not accessible by integration' }) });
    const answer = await github.processor('permissions', { login: 'dhamidi' });
    assert.deepEqual(answer.data.maintainerOf, []);
    assert.ok(lines.includes('permissions of dhamidi on tinyactors-dev/sxml: 403'));
    assert.match(lines.at(-1), /1 unanswered/);
  });

  it('answers github.unavailable when every request failed, or the repositories could not be listed', async () => {
    const down = json(503, {}, { 'retry-after': '30' });
    const all = app({ 'tinyactors-dev/sxml': down, 'tinyactors-dev/tinyactors': json(503, {}, { 'retry-after': '30' }) });
    assert.deepEqual(await all.github.processor('permissions', { login: 'dhamidi' }), { event: 'github.unavailable', data: { retryAfter: 30 } });
    assert.equal(all.lines.filter((line) => / on tinyactors-dev\/.+: 503$/.test(line)).length, 2);
    const unlisted = app({ repositories: json(502, {}) });
    assert.equal((await unlisted.github.processor('permissions', { login: 'dhamidi' })).event, 'github.unavailable');
    assert.ok(unlisted.lines.includes('permissions of dhamidi: the installation\'s repositories: 502'));
  });
});
