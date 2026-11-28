// GitHub through the tinyactors-support GitHub App: the installation's
// reads (repositories, top-level directories, commits, compare, a user's
// permission) with an installation token made from the App's private key,
// and a login's code exchanged for the user's identity (the user's token
// is used for GET /user and dropped). Answers the same events as
// FakeGitHub (lib/processors/standins.mjs).
//
// Unverified against GitHub until the App exists: the endpoints and fields
// are GitHub's documented REST API, not yet called with this App.

import { createSign } from 'node:crypto';

const API = 'https://api.github.com';

export class GitHubApp {
  // log: where a failed permission request is told (server.mjs's log)
  constructor({ appID, clientID, clientSecret, privateKey, org, base, fetch: fetcher = globalThis.fetch, log = () => {} }) {
    Object.assign(this, { appID, clientID, clientSecret, privateKey, org, base, fetch: fetcher, log });
    this.token = null;        // {value, until}
    this.names = new Map();   // repoID -> owner/name, from the last list
  }

  // where /authorize sends a person to sign in
  authorizeURL(state) {
    const q = new URLSearchParams({ client_id: this.clientID, state, redirect_uri: `${this.base}/github/callback` });
    return `https://github.com/login/oauth/authorize?${q}`;
  }

  #jwt() {
    const now = Math.floor(Date.now() / 1000);
    const part = (value) => Buffer.from(JSON.stringify(value)).toString('base64url');
    const unsigned = `${part({ alg: 'RS256', typ: 'JWT' })}.${part({ iat: now - 60, exp: now + 540, iss: String(this.appID) })}`;
    return `${unsigned}.${createSign('RSA-SHA256').update(unsigned).sign(this.privateKey).toString('base64url')}`;
  }

  async #installationToken() {
    if (this.token && this.token.until > Date.now() + 300_000) return this.token.value;
    const app = { authorization: `Bearer ${this.#jwt()}`, accept: 'application/vnd.github+json' };
    const installations = await (await this.fetch(`${API}/app/installations`, { headers: app })).json();
    const installation = installations.find((i) => i.account?.login?.toLowerCase() === this.org.toLowerCase());
    if (!installation) throw new Error(`the App is not installed on ${this.org}`);
    const made = await (await this.fetch(`${API}/app/installations/${installation.id}/access_tokens`,
      { method: 'POST', headers: app })).json();
    this.token = { value: made.token, until: Date.parse(made.expires_at) };
    return made.token;
  }

  async #get(path, { etag } = {}) {
    const headers = { authorization: `Bearer ${await this.#installationToken()}`, accept: 'application/vnd.github+json',
                      'x-github-api-version': '2022-11-28' };
    if (etag) headers['if-none-match'] = etag;
    return this.fetch(`${API}${path}`, { headers, signal: AbortSignal.timeout(15_000) });
  }

  async #name(repoID) {
    if (!this.names.has(repoID)) {
      const response = await this.#get(`/repositories/${repoID}`);
      if (!response.ok) throw Object.assign(new Error(`repository ${repoID}: ${response.status}`), { response });
      this.names.set(repoID, (await response.json()).full_name);
    }
    return this.names.get(repoID);
  }

  static #unavailable(response) {
    const retryAfter = Number(response?.headers?.get('retry-after')) ||
      (response?.headers?.get('x-ratelimit-reset') ? Math.max(1, Number(response.headers.get('x-ratelimit-reset')) - Math.floor(Date.now() / 1000)) : 60);
    return { event: 'github.unavailable', data: { retryAfter: Math.min(retryAfter, 3600) } };
  }

  // The installation's repositories, every page; their names noted for the
  // calls that take a repository by its id. {repos, etag} or {response} on
  // a failure (or {unchanged} for a 304 to `etag`).
  async #repositories(etag) {
    const repos = [];
    let first = null;
    for (let page = 1; page <= 10; page++) {
      const response = await this.#get(`/installation/repositories?per_page=100&page=${page}`, page === 1 ? { etag } : {});
      if (page === 1 && response.status === 304) return { unchanged: true };
      if (!response.ok) return { response };
      first ??= response;
      const listed = (await response.json()).repositories;
      repos.push(...listed.map((r) => ({ id: r.id, name: r.full_name, archived: r.archived })));
      if (listed.length < 100) break;
    }
    for (const r of repos) this.names.set(r.id, r.name);
    return { repos, etag: first.headers.get('etag') };
  }

  processor = async (event, data) => {
    try {
      switch (event) {
        case 'repos.list': {
          const listed = await this.#repositories(data.etag);
          if (listed.unchanged) return { event: 'repos.unchanged', data: {} };
          if (listed.response) return GitHubApp.#unavailable(listed.response);
          return { event: 'repos.listed', data: { repos: listed.repos, etag: listed.etag } };
        }
        case 'contents.list': {
          const response = await this.#get(`/repos/${await this.#name(data.repoID)}/contents/`);
          if (!response.ok) return GitHubApp.#unavailable(response);
          return { event: 'contents', data: { dirs: (await response.json()).filter((e) => e.type === 'dir').map((e) => e.name).sort() } };
        }
        case 'commits.since': {
          const name = await this.#name(data.repoID);
          const response = await this.#get(`/repos/${name}/commits?per_page=100`);
          if (!response.ok) return GitHubApp.#unavailable(response);
          const newestFirst = await response.json(), commits = [];
          for (const c of newestFirst) {
            if (c.sha === data.sha) break;
            commits.push({ sha: c.sha, message: c.commit.message, url: c.html_url });
          }
          return { event: 'commits', data: { commits: commits.reverse() } };
        }
        case 'compare': {
          const response = await this.#get(`/repos/${await this.#name(data.repoID)}/compare/${data.base}...${data.head}`);
          if (response.status === 404) return null;    // a commit GitHub does not have
          if (!response.ok) return GitHubApp.#unavailable(response);
          return { event: 'compared', data: { status: (await response.json()).status } };
        }
        // Which of the installation's repositories the user maintains. The
        // repositories are listed first when this process has not listed them
        // yet (a restart restores the tracker, whose next pull may be minutes
        // away). A failed request is told, never read as "not a maintainer";
        // when every request failed the answer is github.unavailable, so the
        // user keeps the roles it had.
        case 'permissions': {
          if (this.names.size === 0) {
            const listed = await this.#repositories();
            if (listed.response) {
              this.log(`permissions of ${data.login}: the installation's repositories: ${listed.response.status}`);
              return GitHubApp.#unavailable(listed.response);
            }
          }
          const maintainerOf = [];
          const roles = [];
          let failed = 0, last = null;
          await Promise.all([...this.names].map(async ([id, name]) => {
            let response;
            try {
              response = await this.#get(`/repos/${name}/collaborators/${encodeURIComponent(data.login)}/permission`);
            } catch (error) {
              failed++;
              this.log(`permissions of ${data.login} on ${name}: ${error.message}`);
              return;
            }
            if (!response.ok) {
              failed++; last = response;
              this.log(`permissions of ${data.login} on ${name}: ${response.status}`);
              return;
            }
            const { permission, role_name: role } = await response.json();
            roles.push(`${name} ${role ?? permission}`);
            if (permission === 'admin' || role === 'maintain' || role === 'admin') maintainerOf.push(id);
          }));
          maintainerOf.sort((a, b) => a - b);
          if (this.names.size > 0 && failed === this.names.size) return GitHubApp.#unavailable(last);
          this.log(`permissions of ${data.login}: maintains ${maintainerOf.map((id) => this.names.get(id)).join(', ') || 'none'}` +
                   ` of ${this.names.size} repositories${failed ? `, ${failed} unanswered` : ''} (${roles.sort().join(', ')})`);
          return { event: 'permissions', data: { maintainerOf } };
        }
        case 'user.exchange': {
          const exchanged = await this.fetch('https://github.com/login/oauth/access_token', {
            method: 'POST', headers: { accept: 'application/json', 'content-type': 'application/json' },
            body: JSON.stringify({ client_id: this.clientID, client_secret: this.clientSecret, code: data.code }),
          });
          const { access_token: token } = await exchanged.json();
          if (!token) return null;
          const user = await this.fetch(`${API}/user`, { headers: { authorization: `Bearer ${token}`, accept: 'application/vnd.github+json' } });
          if (!user.ok) return null;
          const { id, login } = await user.json();
          return { event: 'user.identified', data: { id, login } };   // the user's token goes no further
        }
      }
    } catch (error) {
      this.log(`github ${event}: ${error.message}`);
      return GitHubApp.#unavailable(error.response);
    }
    return null;
  };
}
