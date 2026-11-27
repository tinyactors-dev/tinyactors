// Stand-ins for the processors that reach outside: GitHub and Jev. The
// tests use them, and so does a server with no GitHub App configured (fake
// repositories and a login that lets you pick who you are). A stand-in is
// `(event, data) => {event, data}` (the answer), null (a failed send) or
// 'silent' (never answered); lib/processors/index.mjs makes it an
// I/O processor.

import { createHash } from 'node:crypto';

export class FakeGitHub {
  constructor() {
    this.repos = [
      { id: 1001, name: 'tinyactors-dev/tinyactors', archived: false },
      { id: 1002, name: 'tinyactors-dev/tinyactors-napi', archived: false },
      { id: 1003, name: 'tinyactors-dev/scxmljs', archived: false },
    ];
    this.dirs = {
      1001: ['docs', 'mise', 'tinyactors-core', 'tinyactors-cpython', 'tinyactors-napi', 'website'],
      1002: ['docs', 'examples', 'lib', 'src', 'test', 'tools'],
      1003: ['conformance', 'examples', 'packages', 'site'],
    };
    // each repository's main, oldest first
    this.commits = {
      1001: [{ sha: 'a1', message: 'chore: the first commit' }],
      1002: [{ sha: 'c1', message: 'feat(napi): the application API' },
             { sha: 'c2', message: 'docs(napi): the services guide' }],
      1003: [{ sha: 's1', message: 'feat: the explorer' }],
    };
    this.users = { 'code-ann': { id: 501, login: 'ann' }, 'code-bob': { id: 502, login: 'bob' },
                   'code-cy': { id: 503, login: 'cy' },
                   'code-dee': { id: 504, login: 'dee' } };
    this.maintainers = { bob: [1001, 1002] };
    this.faults = {};     // event -> how many calls fail with 503 and Retry-After
    this.calls = {};
  }

  commit(repoID, sha, message) { this.commits[repoID].push({ sha, message }); }
  etag() { return createHash('sha256').update(JSON.stringify(this.repos)).digest('hex').slice(0, 12); }

  processor = (event, data) => {
    this.calls[event] = (this.calls[event] ?? 0) + 1;
    if (this.faults[event] > 0) { this.faults[event]--; return { event: 'github.unavailable', data: { retryAfter: 120 } }; }
    switch (event) {
      case 'repos.list':
        return data.etag === this.etag()
          ? { event: 'repos.unchanged', data: {} }
          : { event: 'repos.listed', data: { repos: this.repos, etag: this.etag() } };
      case 'contents.list': return { event: 'contents', data: { dirs: this.dirs[data.repoID] } };
      case 'commits.since': {
        const all = this.commits[data.repoID];
        const from = data.sha === null ? 0 : all.findIndex((c) => c.sha === data.sha) + 1;
        const commits = all.slice(from).map((c) => ({ ...c, url: `https://github.com/${this.name(data.repoID)}/commit/${c.sha}` }));
        return { event: 'commits', data: { commits } };
      }
      case 'compare': {   // GET /repos/{o}/{r}/compare/{base}...{head}: where head is relative to base
        const shas = this.commits[data.repoID].map((c) => c.sha);
        const base = shas.indexOf(data.base), head = shas.indexOf(data.head);
        if (base < 0 || head < 0) return null;       // 404: a commit GitHub does not have
        return { event: 'compared', data: { status: head > base ? 'ahead' : head === base ? 'identical' : 'behind' } };
      }
      case 'user.exchange': {   // POST /login/oauth/access_token, then GET /user; the token is dropped
        const user = this.users[data.code];
        return user ? { event: 'user.identified', data: user } : null;
      }
      case 'permissions':   // GET /repos/{o}/{r}/collaborators/{user}/permission, for each repository
        return { event: 'permissions', data: { maintainerOf: this.maintainers[data.login] ?? [] } };
    }
    return null;
  };

  name(repoID) { return this.repos.find((r) => r.id === repoID).name; }
}

// Jev, TypeSafe's decision model (POST /v1/decisions): given a state and
// typed questions, it picks among the options with probabilities. This
// stand-in decides by word overlap, deterministically, and answers in
// Jev's shape: {model, answers: {key: {type: 'choice', choice,
// confidence, probabilities}}}. Faults: `unavailable` calls answer
// decide.unavailable (a 503), `silent` calls are never answered.
export class FakeJev {
  constructor() { this.requests = []; this.answers = []; this.unavailable = 0; this.silent = 0; }

  processor = (event, data) => {
    if (event !== 'decide') return null;
    this.requests.push(data);
    if (this.unavailable > 0) { this.unavailable--; return { event: 'decide.unavailable', data: { retryAfter: 5 } }; }
    if (this.silent > 0) { this.silent--; return 'silent'; }
    const answers = {};
    for (const [key, q] of Object.entries(data.questions)) {
      answers[key] = key === 'duplicate' ? this.#duplicate(data.state, q) : this.#kind(data.state, q);
    }
    this.answers.push(answers);
    return { event: 'decided', data: { model: 'typesafe/jev-1.14-20261102', answers } };
  };

  // How much of the report the candidate's words cover (and the other way
  // round), against a fixed score for "new".
  #duplicate(state, q) {
    const mine = bag(state.report.title + ' ' + state.report.body);
    const scores = { new: 0.3 };
    for (const p of state.issues) {
      const theirs = bag(p.title + ' ' + p.excerpt);
      const shared = [...mine].filter((w) => theirs.has(w)).length;
      scores[p.id] = shared / Math.min(mine.size, theirs.size || 1) * (p.component === state.report.component ? 1 : 0.6);
    }
    return choice(q.criteria, scores, 10);
  }

  #kind(state, q) {
    const text = (state.report.title + ' ' + state.report.body).toLowerCase();
    const cues = { docs: /\b(docs?|readme|guide|link|documented)\b/, error: /\b(error|message|unclear|misleading|stack)\b/,
                   tooling: /\b(mise|task|build|test|slow|flaky|fails?)\b/, api: /\b(api|method|returns?|throws?)\b/,
                   environment: /\b(container|network|proxy|permission|install)\b/ };
    const scores = Object.fromEntries(Object.keys(q.criteria).map((k) => [k, cues[k]?.test(text) ? 1 : 0]));
    return choice(q.criteria, scores, 3);
  }
}

const STOP = new Set(['the', 'and', 'for', 'with', 'that', 'this', 'when', 'from', 'are', 'not', 'but', 'its', 'has', 'have']);
function bag(text) {
  return new Set(String(text).toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').split(' ').filter((w) => w.length > 2 && !STOP.has(w)));
}

// A softmax over the options' scores: Jev's choice, confidence and probabilities.
function choice(criteria, scores, sharpness) {
  const keys = Object.keys(criteria);
  const weights = keys.map((k) => Math.exp(sharpness * (scores[k] ?? 0)));
  const total = weights.reduce((a, b) => a + b, 0);
  const probabilities = Object.fromEntries(keys.map((k, i) => [k, Math.round(weights[i] / total * 1000) / 1000]));
  const best = keys.reduce((a, b) => (probabilities[b] > probabilities[a] ? b : a));
  return { type: 'choice', choice: best, confidence: probabilities[best], probabilities };
}

// Object storage (R2 in production): the gateway streams uploads into it,
// the charts delete and ask for signed download links. Bytes never reach
