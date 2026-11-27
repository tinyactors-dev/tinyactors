// The gateway: what the server's endpoints do, as functions over the host.
//
// - The MCP tools: reads anyone may make (skills, docs, known issues),
//   tools that need a signed-in caller (report, workaround, attachments,
//   whoami) and tools for a repository's maintainers (triage, close,
//   wontfix). Which a caller may use, the charts decide; the gateway only
//   says who the caller is.
// - The OAuth authorization server the MCP client talks to, which signs
//   people in through the GitHub App (or, with no App configured, the
//   stand-in login: pick who you are, for a private server only).
// - PUT /uploads/<id>: an attachment's bytes, out of band.
//
// It makes every secret (codes, tokens) and hands actors only their
// hashes. A tool call's caller is the user actor's answer, through the
// token's grant; the gateway adds it to the request, and charts trust it.

import { createHash, randomBytes } from 'node:crypto';

const hash = (text) => createHash('sha256').update(text).digest('base64url');
const secret = () => randomBytes(24).toString('base64url');
const id = () => randomBytes(6).toString('hex');
const ISSUE = /^tinyactors:([a-z0-9._-]+):([0-9]+)$/;

const text = (description) => ({ type: 'string', description });
const number = (description) => ({ type: 'number', description });
const object = (properties, required = []) => ({ type: 'object', properties, required, additionalProperties: false });
const issueID = text('the issue, tinyactors:<project>:<n>');

export const TOOLS = {
  // anyone
  list_skills: { access: 'anyone', description: 'The skills for working with tinyactors (each also a resource)',
    input: object({}) },
  get_skill: { access: 'anyone', description: "A skill's SKILL.md", input: object({ name: text('the skill') }, ['name']) },
  search_docs: { access: 'anyone', description: 'Search the documentation of tinyactors',
    input: object({ query: text('words to look for') }, ['query']) },
  get_doc: { access: 'anyone', description: 'A document search_docs found, whole', input: object({ path: text('its path, as search_docs gives it') }, ['path']) },
  known_issues: { access: 'anyone', description: 'The open issues, costliest first, with their workarounds: look here before you report',
    input: object({ repo: text('owner/name, or a project\'s slug; all when absent'), component: text('a top-level directory') }) },
  get_issue: { access: 'anyone', description: 'One issue: its state, workaround, fix and attachments',
    input: object({ id: issueID }, ['id']) },
  // signed in
  whoami: { access: 'user', description: 'Who you are, and what changed for you since your last call', input: object({}) },
  report_issue: { access: 'user',
    description: 'Report what got in your way with tinyactors. The answer says whether it is new, known (with its workaround), fixed (update), or regressed.',
    input: object({
      repo: text('the repository, owner/name (tinyactors-dev/tinyactors for the monorepo)'),
      component: text('its top-level directory (tinyactors-napi, docs, ...); a refusal lists the valid ones'),
      title: text('what went wrong, in a line'),
      body: text('what you did, what happened, what you expected; the relevant lines of output'),
      kind: { type: 'string', enum: ['docs', 'error', 'tooling', 'api', 'environment'] },
      minutes: number('the minutes it cost you'),
      commit: text('the commit you were on (git rev-parse HEAD)'),
      session: text('a link to your session, if you have one'),
      id: text('an issue it is an occurrence of, tinyactors:<project>:<n>'),
    }, ['repo', 'component', 'title']) },
  attach_workaround: { access: 'user', description: 'Tell the next agent how you got past an issue',
    input: object({ id: issueID, text: text('the workaround') }, ['id', 'text']) },
  attach_file: { access: 'user',
    description: 'Open a slot for a file (a log, screenshot, recording, zip, snapshot), then PUT its bytes there with curl: a tool call cannot carry them',
    input: object({ id: issueID, name: text('the file name'), type: text('its media type (text/plain, image/png, application/zip, ...)'),
      size: number('its size in bytes'), sha256: text('its SHA-256, hexadecimal') }, ['id', 'name', 'type', 'size', 'sha256']) },
  get_attachment: { access: 'user', description: "A signed link to one of an issue's attachments",
    input: object({ id: issueID, upload: text('the attachment, as get_issue lists it') }, ['id', 'upload']) },
  // a maintainer of the repository
  triage: { access: 'maintainer', description: 'Accept a new issue, set its kind or title',
    input: object({ id: issueID, kind: text('docs, error, tooling, api or environment'), title: text('a better title') }, ['id']) },
  close: { access: 'maintainer', description: 'Close a fixed issue', input: object({ id: issueID }, ['id']) },
  wontfix: { access: 'maintainer', description: 'Decline an issue, with the reason agents will see',
    input: object({ id: issueID, reason: text('why') }, ['id', 'reason']) },
};

export const INSTRUCTIONS = 'This is the support channel of tinyactors (statecharts as actors; @tinyactors/node, ' +
  'the tinyactors Python package, tinyactors-core). When tinyactors gets in your way (wrong or missing docs, an unclear error, ' +
  'a missing or slow task, a surprising API), call known_issues first: it may have a workaround. If it is not there, ' +
  'report_issue, with the minutes it cost you and the commit you were on. Skills and docs are here too (list_skills, search_docs).';

export class Gateway {
  // support: lib/host.mjs's Support; base: the server's URL; github: a
  // GitHubApp or null (the stand-in login); blobs; content: lib/content.mjs
  constructor({ support, base, github = null, blobs, content }) {
    Object.assign(this, { support, base, github, blobs, content });
    this.misses = [];
  }

  get resource() { return `${this.base}/mcp`; }

  // --- the OAuth authorization server ----------------------------------------

  // GET /authorize: a login actor for this attempt, then off to GitHub.
  async authorize({ clientID, redirectURI, challenge, clientState, resource }) {
    if (resource !== undefined && resource !== this.resource) return { status: 400, error: 'invalid_target' };
    const state = id();
    const answer = await this.support.request('auth', 'login.start',
      { state, client: { clientID, redirectURI, challenge, clientState: clientState ?? '' } });
    if (answer.event !== 'login.started') return { status: 500, error: answer.data?.reason ?? answer.event };
    return { status: 302, location: this.github ? this.github.authorizeURL(state) : `${this.base}/login?state=${state}` };
  }

  // GET /github/callback: GitHub's code; the tracker's own code for the client.
  async githubCallback({ state, code }) {
    if (!/^[0-9a-f]{12}$/.test(String(state))) return { status: 400, error: 'unknown or expired login' };
    const issued = `${state}.${secret()}`;
    const answer = await this.support.request(`login.${state}`, 'callback', { code, codeHash: hash(issued) });
    if (answer.event === 'stale') return { status: 400, error: 'unknown or expired login' };
    if (answer.event !== 'authorized') return { status: 400, error: answer.data?.reason ?? answer.event };
    const { redirectURI, clientState } = answer.data;
    const to = new URL(redirectURI);
    to.searchParams.set('code', issued);
    if (clientState) to.searchParams.set('state', clientState);
    return { status: 302, location: to.toString(), code: issued };
  }

  // POST /token: the code and the PKCE verifier for a grant's tokens.
  async token({ code, verifier, redirectURI }) {
    const [state] = String(code ?? '').split('.');
    if (!/^[0-9a-f]{12}$/.test(state) || !verifier) return { status: 400, error: 'invalid_grant' };
    const grant = { id: id() }, access = secret(), refresh = secret();
    const answer = await this.support.request(`login.${state}`, 'token', {
      codeHash: hash(code), challenge: hash(verifier), redirectURI,
      grant: { id: grant.id, accessHash: hash(access), refreshHash: hash(refresh) } });
    if (answer.event !== 'granted') return { status: 400, error: 'invalid_grant' };
    return { status: 200, access_token: `${grant.id}.${access}`, refresh_token: `${grant.id}.${refresh}`,
             token_type: 'Bearer', expires_in: 3600 };
  }

  // POST /token with grant_type=refresh_token: both secrets rotate; an old
  // refresh token used again revokes the grant (it was copied).
  async refresh({ refreshToken }) {
    const [grantID, old] = String(refreshToken ?? '').split('.');
    if (!/^[0-9a-f]{12}$/.test(grantID)) return { status: 400, error: 'invalid_grant' };
    const access = secret(), refresh = secret();
    const answer = await this.support.request(`grant.${grantID}`, 'refresh',
      { refreshHash: hash(old ?? ''), newAccessHash: hash(access), newRefreshHash: hash(refresh) });
    if (answer.event !== 'refreshed') return { status: 400, error: 'invalid_grant' };
    return { status: 200, access_token: `${grantID}.${access}`, refresh_token: `${grantID}.${refresh}`,
             token_type: 'Bearer', expires_in: 3600 };
  }

  // Who a token stands for (the user's answer, through the grant), or null.
  // A peek leaves the user's news for the next tool call.
  async caller(token, peek = false) {
    const [grantID, access] = String(token ?? '').split('.');
    if (!/^[0-9a-f]{12}$/.test(grantID)) return null;
    const answer = await this.support.request(`grant.${grantID}`, 'authorize', { accessHash: hash(access ?? ''), peek });
    return answer.event === 'caller' ? answer.data : null;
  }

  // --- uploads ------------------------------------------------------------------

  // PUT /uploads/<id>: the bytes of a slot attach_file opened. The upload
  // actor says whether this caller may send them; the gateway streams them
  // into the store, counting and hashing, and stops past the declared
  // size; the actor checks the rest. `chunks` is an async iterable (the
  // request body).
  async upload({ token, uploadID, chunks }) {
    if (!/^[0-9]+\.[0-9]+\.[0-9]+$/.test(String(uploadID))) return { status: 404, error: 'no such upload' };
    const caller = await this.caller(token, true);
    if (!caller) return { status: 401 };
    const go = await this.support.request(`upload.${uploadID}`, 'upload.begin', { githubID: caller.githubID });
    if (go.event === 'stale') return { status: 404, error: 'no such upload, or it ended' };
    if (go.event !== 'upload.go') return { status: 403, error: go.data?.reason };
    const digest = createHash('sha256'), parts = [];
    let size = 0;
    for await (const chunk of chunks) {
      size += chunk.length;
      if (size > go.data.maxBytes) {
        this.support.post(`upload.${uploadID}`, 'upload.aborted', { reason: 'more bytes than declared' });
        return { status: 413, error: 'more bytes than declared' };
      }
      digest.update(chunk); parts.push(chunk);
    }
    this.blobs.put(go.data.key, Buffer.concat(parts));
    this.support.post(`upload.${uploadID}`, 'received', { size, sha256: digest.digest('hex') });
    return { status: 201, message: 'received: the file is checked now; get_issue shows it once it is attached' };
  }

  // --- MCP ----------------------------------------------------------------------

  needsCaller(name) { return TOOLS[name] && TOOLS[name].access !== 'anyone'; }

  // A tool call: {result} or {error, status}. `caller` is null for none.
  async callTool(name, args = {}, caller = null) {
    const tool = TOOLS[name];
    if (!tool) return { status: 404, error: `no tool ${name}` };
    if (tool.access !== 'anyone' && !caller) return { status: 401, error: 'sign in first' };
    if (args.id !== undefined) {
      const named = ISSUE.exec(String(args.id));
      if (!named) return { status: 400, error: `not an issue identifier: ${args.id}` };
      const { id: _, ...rest } = args;
      args = name === 'report_issue' ? { ...rest, n: Number(named[2]) } : { ...rest, repo: named[1], n: Number(named[2]) };
    }
    const result = await this.#tool(name, args, caller);
    const answered = caller && caller.unseen?.length ? { ...result, sinceYourLastVisit: caller.unseen } : result;
    return { result: this.#named(answered, await this.#repositories()) };
  }

  // What each project's slug is called now: an issue's identifier never
  // changes, its repository's name may (the tracker keeps every name).
  async #repositories() {
    const { data } = await this.support.request('tracker', 'projects.list', {});
    return Object.fromEntries((data?.projects ?? []).map((p) => [p.slug, p.name]));
  }

  #named(value, names) {
    if (Array.isArray(value)) return value.map((v) => this.#named(v, names));
    if (value === null || typeof value !== 'object') return value;
    const out = Object.fromEntries(Object.entries(value).map(([k, v]) => [k, this.#named(v, names)]));
    const named = ISSUE.exec(out.id ?? '');
    return named && names[named[1]] ? { id: out.id, repo: names[named[1]], ...out } : out;
  }

  async #tool(name, args, caller) {
    const s = this.support;
    const at = new Date(s.wallNow()).toISOString();
    switch (name) {
      // reads that wake no actor
      case 'list_skills': return { skills: this.content.skills.map(({ name, description }) => ({ name, description, uri: `skill://${name}` })) };
      case 'get_skill': return this.content.skills.find((k) => k.name === args.name) ?? { answer: 'refused', reason: `no skill ${args.name}` };
      case 'search_docs': {
        const hits = this.content.search(String(args.query ?? ''));
        if (hits.length === 0) this.misses.push(String(args.query).toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').trim());
        return { hits };
      }
      case 'get_doc': {
        const doc = this.content.doc(String(args.path ?? ''));
        return doc ? { path: doc.path, title: doc.title, text: doc.text } : { answer: 'refused', reason: `no document ${args.path}` };
      }
      // reads from the projects' indexes: one actor per repository
      case 'known_issues': {
        const { data } = await s.request('tracker', 'projects.list', {});
        const repos = (data?.projects ?? []).filter((p) => !p.archived && (!args.repo || p.name === args.repo || p.slug === args.repo));
        const lists = await Promise.all(repos.map((p) => s.request('tracker', 'issues.list', { repo: p.name, open: true, component: args.component })));
        return { issues: lists.flatMap((a) => a.event === 'issues' ? a.data.issues.map((i) => ({ repo: a.data.repo, ...i })) : []) };
      }
      case 'get_issue': return this.#answer(await s.request('tracker', 'issue.get', { ...args, caller }));
      case 'whoami': return { login: caller.login, maintainerOf: caller.maintainerOf, quota: caller.quota };
      case 'report_issue': return this.#answer(await s.request('tracker', 'report', { ...args, at, caller }));
      case 'attach_workaround': return this.#answer(await s.request('tracker', 'workaround', { ...args, caller }));
      case 'attach_file': {
        const answer = this.#answer(await s.request('tracker', 'attach.open', { ...args, caller }));
        if (answer.answer !== 'upload.opened') return answer;
        const { path, ...slot } = answer;
        return { ...slot, uploadURL: `${this.base}${path}`,
                 how: `curl -T <file> -H "Authorization: Bearer <your access token>" ${this.base}${path}` };
      }
      case 'get_attachment': return this.#answer(await s.request('tracker', 'attachment.get', { ...args, caller }));
      case 'triage': case 'close': case 'wontfix':
        return this.#answer(await s.request('tracker', name, { ...args, caller }));
    }
    return { answer: 'refused', reason: `no tool ${name}` };
  }

  #answer({ event, data }) { return { answer: event, ...(data ?? {}) }; }
}
