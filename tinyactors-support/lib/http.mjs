// The HTTP server: MCP over Streamable HTTP at /mcp (stateless: each POST
// is one JSON-RPC exchange, answered as JSON), the OAuth authorization
// server around it, the stand-in login (no GitHub App configured), PUT
// /uploads/<id>, signed downloads at /files/<key>, and /healthz.
//
// OAuth, as MCP's authorization asks (protected resource metadata, RFC
// 9728; authorization server metadata, RFC 8414; PKCE with S256; the
// resource parameter, RFC 8707) with both ways a client identifies
// itself: dynamic client registration (RFC 7591, POST /register) and a
// client ID metadata document (a client_id that is an https URL whose
// JSON names it and its redirect URIs). Claude's connectors use either.

import { createServer } from 'node:http';
import { randomBytes } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { CallToolRequestSchema, ListResourcesRequestSchema, ListToolsRequestSchema, ReadResourceRequestSchema }
  from '@modelcontextprotocol/sdk/types.js';
import { INSTRUCTIONS, TOOLS } from './gateway.mjs';
import { APP_URI, summarize } from './tools.mjs';
import { APP } from './app/index.mjs';

const VERSION = '0.1.0';

// A tool's result as MCP carries it: its structured content (the data, per
// its outputSchema) and a line of text for people; an error is both too.
export async function toolResult(gateway, caller, name, args) {
  const answer = await gateway.callTool(name, args ?? {}, caller);
  const structured = answer.error ? { error: answer.error } : answer.result;
  return { content: [{ type: 'text', text: summarize(name, structured) }], structuredContent: structured,
           ...(answer.error ? { isError: true } : {}) };
}
const LIMIT = 1024 * 1024;   // a JSON or form body; uploads stream past it

// The OAuth clients: registered ones in SQLite, metadata documents fetched and kept a while.
class Clients {
  constructor(path, fetcher = globalThis.fetch) {
    this.database = new DatabaseSync(path);
    this.database.exec('CREATE TABLE IF NOT EXISTS clients (id TEXT PRIMARY KEY, metadata TEXT NOT NULL, at INTEGER NOT NULL)');
    this.fetch = fetcher;
    this.documents = new Map();   // url -> {metadata, until}
  }
  register(metadata) {
    const uris = metadata.redirect_uris;
    if (!Array.isArray(uris) || uris.length === 0 || !uris.every(okRedirect)) return null;
    const id = `client-${randomBytes(12).toString('hex')}`;
    const kept = { client_name: String(metadata.client_name ?? ''), redirect_uris: uris.map(String) };
    this.database.prepare('INSERT INTO clients (id, metadata, at) VALUES (?, ?, ?)').run(id, JSON.stringify(kept), Date.now());
    return { client_id: id, client_id_issued_at: Math.floor(Date.now() / 1000), ...kept,
             token_endpoint_auth_method: 'none', grant_types: ['authorization_code', 'refresh_token'], response_types: ['code'] };
  }
  async find(clientID) {
    if (/^https:\/\//.test(clientID)) return this.#document(clientID);
    const row = this.database.prepare('SELECT metadata FROM clients WHERE id = ?').get(String(clientID));
    return row ? JSON.parse(row.metadata) : null;
  }
  async #document(url) {
    const kept = this.documents.get(url);
    if (kept && kept.until > Date.now()) return kept.metadata;
    const host = new URL(url).hostname;
    if (host === 'localhost' || /^(\d+\.){3}\d+$/.test(host) || host.includes(':')) return null;   // names only, never an address
    try {
      const response = await this.fetch(url, { signal: AbortSignal.timeout(5000), redirect: 'error', headers: { accept: 'application/json' } });
      if (!response.ok) return null;
      const metadata = await response.json();
      if (metadata.client_id !== url || !Array.isArray(metadata.redirect_uris) || !metadata.redirect_uris.every(okRedirect)) return null;
      this.documents.set(url, { metadata, until: Date.now() + 600_000 });
      return metadata;
    } catch { return null; }
  }
}

function okRedirect(uri) {
  try {
    const url = new URL(String(uri));
    return url.protocol === 'https:' || (url.protocol === 'http:' && ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname));
  } catch { return false; }
}

async function body(request, limit = LIMIT) {
  const parts = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > limit) throw Object.assign(new Error('too large'), { status: 413 });
    parts.push(chunk);
  }
  return Buffer.concat(parts).toString('utf8');
}

async function fields(request) {
  const text = await body(request);
  if ((request.headers['content-type'] ?? '').includes('application/json')) return text ? JSON.parse(text) : {};
  return Object.fromEntries(new URLSearchParams(text));
}

function send(response, status, value, headers = {}) {
  const json = typeof value === 'string' ? value : JSON.stringify(value);
  response.writeHead(status, { 'content-type': typeof value === 'string' ? 'text/html; charset=utf-8' : 'application/json',
                               'cache-control': 'no-store', ...headers });
  response.end(json);
}

const escape = (text) => String(text).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);

// The stand-in login: who you are, picked from the stand-in GitHub's users.
function loginPage(state, users) {
  const links = users.map((u) => `<li><a href="/login/choose?state=${encodeURIComponent(state)}&amp;login=${encodeURIComponent(u)}">${escape(u)}</a></li>`).join('');
  return `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Sign in: tinyactors support</title>
<body style="font: 16px system-ui; max-width: 32rem; margin: 3rem auto; padding: 0 1rem">
<h1>Sign in (stand-in)</h1><p>This server has no GitHub App yet: pick who you are. Each is a user of the stand-in GitHub; bob maintains its tinyactors and tinyactors-napi.</p>
<ul>${links}</ul></body>`;
}

export function serve({ support, gateway, port = 0, clientsPath = ':memory:', standinUsers = null, log = () => {} }) {
  const clients = new Clients(clientsPath);
  const base = () => gateway.base;
  const challenge = () => `Bearer resource_metadata="${base()}/.well-known/oauth-protected-resource"`;

  function mcpServer(caller) {
    const server = new Server({ name: 'tinyactors-support', version: VERSION },
      { capabilities: { tools: {}, resources: {} }, instructions: INSTRUCTIONS });
    server.setRequestHandler(ListToolsRequestSchema, async () => ({
      tools: Object.entries(TOOLS).map(([name, t]) => ({ name, description: t.description, inputSchema: t.input,
                                                          outputSchema: t.output, ...(t.meta ? { _meta: t.meta } : {}) })) }));
    // A result is its structured content and a line of text; JSON only in
    // the structured content (lib/tools.mjs).
    server.setRequestHandler(CallToolRequestSchema, async (request) =>
      toolResult(gateway, caller, request.params.name, request.params.arguments));
    server.setRequestHandler(ListResourcesRequestSchema, async () => ({
      resources: [
        { uri: APP_URI, name: 'Issues', description: 'The issue list: filter, open, report, triage, close', mimeType: APP.mimeType },
        ...gateway.content.skills.flatMap((k) => [
          { uri: `skill://${k.name}`, name: k.name, description: k.description, mimeType: 'text/markdown' },
          ...k.files.map((f) => ({ uri: `skill://${k.name}/${f.path}`, name: `${k.name}/${f.path}`, mimeType: 'text/markdown' })),
        ]),
      ] }));
    server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
      if (request.params.uri === APP_URI)
        return { contents: [{ uri: APP_URI, mimeType: APP.mimeType, text: APP.html, _meta: APP.meta }] };
      const file = gateway.content.skills.flatMap((k) => [{ uri: `skill://${k.name}`, text: k.text },
        ...k.files.map((f) => ({ uri: `skill://${k.name}/${f.path}`, text: f.text }))]).find((f) => f.uri === request.params.uri);
      if (!file) throw new Error(`no resource ${request.params.uri}`);
      return { contents: [{ uri: request.params.uri, mimeType: 'text/markdown', text: file.text }] };
    });
    return server;
  }

  async function mcp(request, response) {
    if (request.method !== 'POST') return send(response, 405, { error: 'POST only: this server is stateless' }, { allow: 'POST' });
    const text = await body(request);
    let message;
    try { message = JSON.parse(text); } catch { return send(response, 400, { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'not JSON' } }); }
    const token = /^Bearer (.+)$/i.exec(request.headers.authorization ?? '')?.[1];
    const calls = Array.isArray(message) ? message : [message];
    // The user's news since their last call goes out with a tool's result:
    // any other request (initialize, tools/list) only peeks, leaving it.
    const caller = token ? await gateway.caller(token, !calls.some((m) => m?.method === 'tools/call')) : null;
    if (token && !caller) return send(response, 401, { error: 'invalid_token' }, { 'www-authenticate': `${challenge()}, error="invalid_token"` });
    if (!caller && calls.some((m) => m?.method === 'tools/call' && gateway.needsCaller(m.params?.name)))
      return send(response, 401, { error: 'sign in to use this tool' }, { 'www-authenticate': challenge() });
    const server = mcpServer(caller);
    const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined, enableJsonResponse: true });
    response.on('close', () => { transport.close(); server.close(); });
    await server.connect(transport);
    await transport.handleRequest(request, response, message);
  }

  async function route(request, response) {
    const url = new URL(request.url, 'http://local');
    const path = url.pathname, query = Object.fromEntries(url.searchParams);
    if (path === '/mcp') return mcp(request, response);
    if (path === '/healthz') return send(response, 200, { ok: true, restored: support.restored });
    if (path === '/') return send(response, 200, `<!doctype html><meta charset="utf-8"><title>tinyactors support</title>
<p>The support channel of tinyactors for agents: connect an MCP client to <code>${escape(base())}/mcp</code>.</p>`);
    if (path === '/.well-known/oauth-protected-resource' || path === '/.well-known/oauth-protected-resource/mcp')
      return send(response, 200, { resource: gateway.resource, authorization_servers: [base()], bearer_methods_supported: ['header'],
                                   resource_name: 'tinyactors support' });
    if (path === '/.well-known/oauth-authorization-server' || path === '/.well-known/openid-configuration')
      return send(response, 200, {
        issuer: base(), authorization_endpoint: `${base()}/authorize`, token_endpoint: `${base()}/token`,
        registration_endpoint: `${base()}/register`, response_types_supported: ['code'],
        grant_types_supported: ['authorization_code', 'refresh_token'], code_challenge_methods_supported: ['S256'],
        token_endpoint_auth_methods_supported: ['none'], client_id_metadata_document_supported: true });
    if (path === '/register' && request.method === 'POST') {
      const registered = clients.register(await fields(request));
      return registered ? send(response, 201, registered) : send(response, 400, { error: 'invalid_redirect_uri' });
    }
    if (path === '/authorize') {
      if (query.response_type !== 'code' || query.code_challenge_method !== 'S256' || !query.code_challenge)
        return send(response, 400, { error: 'invalid_request', error_description: 'response_type=code and PKCE with S256' });
      const client = await clients.find(query.client_id ?? '');
      if (!client) return send(response, 400, { error: 'invalid_client' });
      if (!client.redirect_uris.includes(query.redirect_uri)) return send(response, 400, { error: 'invalid_redirect_uri' });
      const started = await gateway.authorize({ clientID: query.client_id, redirectURI: query.redirect_uri,
        challenge: query.code_challenge, clientState: query.state, resource: query.resource });
      return started.status === 302 ? send(response, 302, '', { location: started.location }) : send(response, started.status, { error: started.error });
    }
    if (path === '/login' && standinUsers) return send(response, 200, loginPage(query.state ?? '', standinUsers));
    if (path === '/login/choose' && standinUsers) {
      if (!standinUsers.includes(query.login)) return send(response, 400, { error: 'no such user' });
      return send(response, 302, '', { location: `${base()}/github/callback?state=${encodeURIComponent(query.state ?? '')}&code=code-${encodeURIComponent(query.login)}` });
    }
    if (path === '/github/callback') {
      const back = await gateway.githubCallback({ state: query.state, code: query.code });
      return back.status === 302 ? send(response, 302, '', { location: back.location }) : send(response, back.status, { error: back.error });
    }
    if (path === '/token' && request.method === 'POST') {
      const form = await fields(request);
      const answer = form.grant_type === 'authorization_code'
        ? await gateway.token({ code: form.code, verifier: form.code_verifier, redirectURI: form.redirect_uri })
        : form.grant_type === 'refresh_token' ? await gateway.refresh({ refreshToken: form.refresh_token })
        : { status: 400, error: 'unsupported_grant_type' };
      const { status, ...rest } = answer;
      return send(response, status, rest);
    }
    const upload = /^\/uploads\/([0-9.]+)$/.exec(path);
    if (upload && request.method === 'PUT') {
      const token = /^Bearer (.+)$/i.exec(request.headers.authorization ?? '')?.[1];
      const { status, ...rest } = await gateway.upload({ token, uploadID: upload[1], chunks: request });
      if (!request.readableEnded) request.resume();   // a refusal before the body: drain it, so the client reads the answer
      if (status === 401) return send(response, 401, { error: 'invalid_token' }, { 'www-authenticate': challenge() });
      return send(response, status, rest);
    }
    const file = /^\/files\/(.+)$/.exec(path);
    if (file && request.method === 'GET') {
      const key = file[1].split('/').map(decodeURIComponent).join('/');
      const bytes = gateway.blobs.verify(key, query.expires, query.signature) && gateway.blobs.get(key);
      if (!bytes) return send(response, 404, { error: 'no such file, or the link expired' });
      response.writeHead(200, { 'content-type': 'application/octet-stream', 'x-content-type-options': 'nosniff',
        'content-disposition': `attachment; filename="${key.split('/').pop().replace(/[^A-Za-z0-9._-]/g, '_')}"`,
        'content-security-policy': "default-src 'none'", 'cache-control': 'private, no-store' });
      return response.end(bytes);
    }
    return send(response, 404, { error: 'not found' });
  }

  const server = createServer((request, response) => {
    route(request, response).catch((error) => {
      log(`${request.method} ${request.url}: ${error.stack ?? error}`);
      if (!response.headersSent) send(response, error.status ?? 500, { error: error.status ? error.message : 'internal error' });
      else response.end();
    });
  });
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}
