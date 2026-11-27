// The server over HTTP, as an MCP client and an agent's curl see it: the
// discovery documents, MCP's initialize, tools/list and tools/call, the 401
// that starts a login, client registration, the whole OAuth flow through
// the stand-in login, a report, an upload and a signed download.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { after, before, describe, it } from 'node:test';
import { serve } from '../lib/http.mjs';
import { stack, sha } from './stack.mjs';

describe('the server over HTTP', () => {
  let s, server, base, tokens;
  const redirectURI = 'https://claude.ai/api/mcp/auth_callback';

  before(async () => {
    s = await stack();
    server = await serve({ support: s.support, gateway: s.gateway, standinUsers: ['ann', 'bob', 'cy', 'dee'] });
    base = `http://127.0.0.1:${server.address().port}`;
    s.gateway.base = base;
    s.blobs.base = base;
  });
  after(async () => { server.close(); await s.support.close(); });

  let id = 0;
  async function rpc(method, params, token) {
    const response = await fetch(`${base}/mcp`, { method: 'POST',
      headers: { 'content-type': 'application/json', accept: 'application/json, text/event-stream',
                 ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body: JSON.stringify({ jsonrpc: '2.0', id: ++id, method, params }) });
    const text = await response.text();
    return { status: response.status, headers: response.headers, body: text ? JSON.parse(text) : null };
  }
  const call = async (name, args, token) => {
    const { status, body, headers } = await rpc('tools/call', { name, arguments: args }, token);
    return { status, headers, value: body?.result ? JSON.parse(body.result.content[0].text) : body };
  };

  it('describes itself for discovery: the protected resource and its authorization server', async () => {
    const resource = await (await fetch(`${base}/.well-known/oauth-protected-resource`)).json();
    assert.equal(resource.resource, `${base}/mcp`);
    assert.deepEqual(resource.authorization_servers, [base]);
    const server = await (await fetch(`${base}/.well-known/oauth-authorization-server`)).json();
    assert.deepEqual(server.code_challenge_methods_supported, ['S256']);
    assert.equal(server.registration_endpoint, `${base}/register`);
    assert.equal(server.client_id_metadata_document_supported, true);
    assert.deepEqual(server.token_endpoint_auth_methods_supported, ['none']);
  });

  it('speaks MCP to anyone: initialize, tools, resources, a read', async () => {
    const init = await rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'test', version: '1' } });
    assert.equal(init.status, 200);
    assert.equal(init.body.result.serverInfo.name, 'tinyactors-support');
    assert.match(init.body.result.instructions, /known_issues first/);
    const tools = (await rpc('tools/list', {})).body.result.tools;
    assert.ok(tools.some((t) => t.name === 'report_issue' && t.inputSchema.required.includes('title')));
    const resources = (await rpc('resources/list', {})).body.result.resources;
    assert.deepEqual(resources.map((r) => r.uri), ['skill://reporting-tinyactors-issues']);
    const skill = (await rpc('resources/read', { uri: 'skill://reporting-tinyactors-issues' })).body.result.contents[0];
    assert.match(skill.text, /# Reporting tinyactors issues/);
    assert.deepEqual((await call('known_issues', {})).value.issues, []);
  });

  it('answers a tool that needs a login with 401 and where to find the authorization server', async () => {
    const refused = await call('report_issue', { repo: 'tinyactors-dev/tinyactors', component: 'docs', title: 'x' });
    assert.equal(refused.status, 401);
    assert.equal(refused.headers.get('www-authenticate'), `Bearer resource_metadata="${base}/.well-known/oauth-protected-resource"`);
    const forged = await rpc('tools/list', {}, '0123456789ab.forged');
    assert.equal(forged.status, 401);
    assert.match(forged.headers.get('www-authenticate'), /invalid_token/);
  });

  it('signs a person in: registration, authorize, the stand-in login, the callback, the token', async () => {
    const registered = await (await fetch(`${base}/register`, { method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ client_name: 'Claude', redirect_uris: [redirectURI] }) })).json();
    assert.match(registered.client_id, /^client-/);
    assert.equal((await fetch(`${base}/register`, { method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ redirect_uris: ['javascript:alert(1)'] }) })).status, 400);

    const verifier = 'a-verifier-of-enough-length-0123456789abcdefghijklmnop';
    const query = new URLSearchParams({ response_type: 'code', client_id: registered.client_id, redirect_uri: redirectURI,
      code_challenge: createHash('sha256').update(verifier).digest('base64url'), code_challenge_method: 'S256',
      state: 'client-state', resource: `${base}/mcp` });
    const wrongRedirect = new URLSearchParams(query); wrongRedirect.set('redirect_uri', 'https://evil.example/cb');
    assert.equal((await fetch(`${base}/authorize?${wrongRedirect}`, { redirect: 'manual' })).status, 400);
    const unknown = new URLSearchParams(query); unknown.set('client_id', 'https://client.invalid/metadata.json');
    assert.equal((await fetch(`${base}/authorize?${unknown}`, { redirect: 'manual' })).status, 400);

    const toLogin = await fetch(`${base}/authorize?${query}`, { redirect: 'manual' });
    assert.equal(toLogin.status, 302);
    const login = new URL(toLogin.headers.get('location'));
    assert.equal(login.pathname, '/login');
    assert.match(await (await fetch(login)).text(), />ann</);
    const chosen = await fetch(`${base}/login/choose?state=${login.searchParams.get('state')}&login=ann`, { redirect: 'manual' });
    const callback = await fetch(chosen.headers.get('location'), { redirect: 'manual' });
    assert.equal(callback.status, 302);
    const back = new URL(callback.headers.get('location'));
    assert.equal(`${back.origin}${back.pathname}`, redirectURI);
    assert.equal(back.searchParams.get('state'), 'client-state');

    const exchange = (fields) => fetch(`${base}/token`, { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(fields) });
    const wrong = await exchange({ grant_type: 'authorization_code', code: back.searchParams.get('code'), code_verifier: 'not-it', redirect_uri: redirectURI });
    assert.equal(wrong.status, 400);   // and the code is spent: a code is tried once
    // again from the start, the right way
    const again = await fetch(`${base}/authorize?${query}`, { redirect: 'manual' });
    const state = new URL(again.headers.get('location')).searchParams.get('state');
    const code = new URL((await fetch((await fetch(`${base}/login/choose?state=${state}&login=ann`, { redirect: 'manual' })).headers.get('location'),
      { redirect: 'manual' })).headers.get('location')).searchParams.get('code');
    const issued = await exchange({ grant_type: 'authorization_code', code, code_verifier: verifier, redirect_uri: redirectURI });
    assert.equal(issued.status, 200);
    tokens = await issued.json();
    assert.equal(tokens.token_type, 'Bearer');
    assert.equal((await call('whoami', {}, tokens.access_token)).value.login, 'ann');
    const refreshed = await (await exchange({ grant_type: 'refresh_token', refresh_token: tokens.refresh_token })).json();
    assert.ok(refreshed.access_token && refreshed.access_token !== tokens.access_token);
    tokens = refreshed;
  });

  it('files a report, takes an attachment by PUT, and serves it back through a signed link', async () => {
    const filed = await call('report_issue', { repo: 'tinyactors-dev/tinyactors-napi', component: 'examples', minutes: 10,
      title: 'mise run examples fails before mise run build', body: 'cannot find build/tinyactors.node' }, tokens.access_token);
    assert.equal(filed.value.answer, 'filed');
    assert.equal(filed.value.id, 'tinyactors:tinyactors-napi:1');
    const log = Buffer.from('build failed\nAuthorization: Bearer ghp_0123456789abcdefghijABCDEFGHIJ0123\n');
    const slot = (await call('attach_file', { id: filed.value.id, name: 'build.log', type: 'text/plain', size: log.length, sha256: sha(log) },
      tokens.access_token)).value;
    assert.equal(slot.uploadURL, `${base}/uploads/1002.1.1`);
    assert.equal((await fetch(slot.uploadURL, { method: 'PUT', body: log })).status, 401);
    const put = await fetch(slot.uploadURL, { method: 'PUT', body: log, headers: { authorization: `Bearer ${tokens.access_token}` } });
    assert.equal(put.status, 201);
    await s.idle();
    const issue = (await call('get_issue', { id: filed.value.id }, tokens.access_token)).value;
    assert.equal(issue.attachments[0].scan.verdict, 'redacted');
    const link = (await call('get_attachment', { id: filed.value.id, upload: issue.attachments[0].upload }, tokens.access_token)).value;
    const download = await fetch(link.url);
    assert.equal(download.status, 200);
    assert.equal(download.headers.get('content-disposition'), 'attachment; filename="build.log.redacted"');
    assert.equal(await download.text(), 'build failed\n[REDACTED]\n');
    const tampered = new URL(link.url); tampered.searchParams.set('signature', 'forged');
    assert.equal((await fetch(tampered)).status, 404);
  });

  it('is healthy', async () => {
    assert.deepEqual(await (await fetch(`${base}/healthz`)).json(), { ok: true, restored: false });
  });
});
