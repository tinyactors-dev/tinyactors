// The catalog: every issue in full, in one SQLite database, with the
// ledger that is its history. An issue actor keeps only what its
// decisions need; everything a reader asks for is here, read without
// waking an actor, and searched with SQLite's FTS5.
//
// One database, one transaction per change: the `catalog` I/O processor
// appends the change to the ledger and applies it to the catalog's rows
// in the same transaction, before the issue answers (it waits for
// `ledger.written`). So the two never disagree, and the catalog's rows
// are a projection of the ledger: `rebuild()` replays the ledger into
// them (a projection fixed, a schema changed).
//
// Tables:
//   ledger       every change: seq, at (the host's wall clock), kind, key, record (JSON)
//   projects     the repositories (the tracker's inventory): repoID, slug, name, names, archived
//   issues       an issue's current state, one row per identifier, deleted ones marked
//   reports      every report of an issue: when, from which session and commit, by whom, minutes
//   attachments  each attached file's description and its key in the blob store
//   issues_fts   FTS5 over title, body and workaround (porter stemming)

import { DatabaseSync } from 'node:sqlite';
import { existsSync } from 'node:fs';

const ISSUE = /^tinyactors:([a-z0-9._-]+):([0-9]+)$/;
const OPEN = ['new', 'triaged', 'regressed'];
const STATES = ['new', 'triaged', 'regressed', 'fixed', 'closed', 'wontfix'];

const SCHEMA = `
  CREATE TABLE IF NOT EXISTS ledger (seq INTEGER PRIMARY KEY, at TEXT NOT NULL, kind TEXT NOT NULL,
    key TEXT NOT NULL, record TEXT NOT NULL);
  CREATE TABLE IF NOT EXISTS meta (name TEXT PRIMARY KEY, value TEXT NOT NULL);
  CREATE TABLE IF NOT EXISTS projects (repoID TEXT PRIMARY KEY, slug TEXT NOT NULL UNIQUE, name TEXT NOT NULL,
    names TEXT NOT NULL DEFAULT '[]', archived INTEGER NOT NULL DEFAULT 0);
  CREATE TABLE IF NOT EXISTS issues (id TEXT PRIMARY KEY, repoID TEXT, slug TEXT NOT NULL, n INTEGER NOT NULL,
    title TEXT NOT NULL, body TEXT NOT NULL DEFAULT '', component TEXT NOT NULL, kind TEXT NOT NULL,
    state TEXT NOT NULL, count INTEGER NOT NULL DEFAULT 0, minutes INTEGER NOT NULL DEFAULT 0,
    workaround TEXT, workaroundBy TEXT, fixSha TEXT, fixURL TEXT, fixRepoID INTEGER, reason TEXT,
    possible TEXT, filed TEXT, updated TEXT, purged INTEGER NOT NULL DEFAULT 0, reporters TEXT NOT NULL DEFAULT '[]',
    deleted INTEGER NOT NULL DEFAULT 0, deletedBy TEXT, deletedReason TEXT, UNIQUE (slug, n));
  CREATE TABLE IF NOT EXISTS reports (id TEXT NOT NULL, seq INTEGER NOT NULL, at TEXT NOT NULL, session TEXT,
    "commit" TEXT, reporter TEXT, reporterID INTEGER, minutes INTEGER NOT NULL DEFAULT 0);
  CREATE INDEX IF NOT EXISTS reports_by_issue ON reports (id, seq);
  CREATE TABLE IF NOT EXISTS attachments (upload TEXT PRIMARY KEY, id TEXT NOT NULL, name TEXT, type TEXT,
    kind TEXT, size INTEGER, sha256 TEXT, key TEXT, by TEXT, byID INTEGER, scan TEXT);
  CREATE VIRTUAL TABLE IF NOT EXISTS issues_fts USING fts5(id UNINDEXED, title, body, workaround,
    tokenize = 'porter unicode61');
`;

// The words of a text as FTS5 terms, quoted (no operators from the reporter).
function terms(text, limit = 32) {
  const words = String(text ?? '').toLowerCase().match(/[\p{L}\p{N}_]{3,}/gu) ?? [];
  return [...new Set(words)].slice(0, limit).map((w) => `"${w}"`);
}

export class Catalog {
  // path: the database file (':memory:' in tests); now: the host's wall clock in milliseconds
  constructor(path = ':memory:', { now = Date.now } = {}) {
    this.now = now;
    this.path = path;
    this.database = new DatabaseSync(path);
    this.database.exec('PRAGMA journal_mode = WAL; PRAGMA busy_timeout = 5000;');
    this.database.exec(SCHEMA);
    this.failing = 0;   // tests: the next appends fail, as a full disk would
    const db = this.database;
    this.sql = {
      append: db.prepare('INSERT INTO ledger (at, kind, key, record) VALUES (?, ?, ?, ?)'),
      issue: db.prepare('SELECT * FROM issues WHERE id = ?'),
      repoOf: db.prepare('SELECT repoID FROM projects WHERE slug = ?'),
      ftsDelete: db.prepare('DELETE FROM issues_fts WHERE id = ?'),
      ftsInsert: db.prepare('INSERT INTO issues_fts (id, title, body, workaround) VALUES (?, ?, ?, ?)'),
      report: db.prepare('INSERT INTO reports (id, seq, at, session, "commit", reporter, reporterID, minutes) VALUES (?, ?, ?, ?, ?, ?, ?, ?)'),
    };
  }

  close() { this.database.close(); }

  #transaction(work) {
    this.database.exec('BEGIN IMMEDIATE');
    try { const out = work(); this.database.exec('COMMIT'); return out; }
    catch (error) { this.database.exec('ROLLBACK'); throw error; }
  }

  // --- writing: a change, into the ledger and the catalog at once -------------

  // Appends a change and applies it; answers {seq, at}.
  append(kind, key, record, at = new Date(this.now()).toISOString()) {
    return this.#transaction(() => {
      const { lastInsertRowid } = this.sql.append.run(at, kind, key, JSON.stringify(record ?? null));
      const seq = Number(lastInsertRowid);
      this.#apply({ seq, at, kind, key, record: record ?? {} });
      return { seq, at };
    });
  }

  // What a change does to the catalog's rows. Replayed by rebuild(): a pure
  // function of the ledger row (and the projects table, for an old row
  // that does not name its repository).
  #apply({ seq, at, kind, key, record: r }) {
    const named = ISSUE.exec(key);
    if (!named) return;   // a decision, keyed by its project and title
    const [, slug, number] = named, n = Number(number);
    const db = this.database;
    const set = (fields) => {
      const names = Object.keys(fields);
      db.prepare(`UPDATE issues SET ${names.map((f) => `${f} = ?`).join(', ')} WHERE id = ?`).run(...names.map((f) => fields[f]), key);
    };
    const touched = kind === 'migrated' ? {} : { updated: at };
    const report = (d) => this.sql.report.run(key, seq, at, d.session ?? null, d.commit ?? null, d.reporter ?? d.caller ?? null,
                                              d.reporterID ?? null, Math.round(d.minutes ?? 0));
    switch (kind) {
      case 'filed': {
        const repoID = r.repoID ?? this.sql.repoOf.get(slug)?.repoID ?? null;
        db.prepare(`INSERT OR IGNORE INTO issues (id, repoID, slug, n, title, body, component, kind, state, count, minutes,
                      possible, filed, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'new', 1, ?, ?, ?, ?)`)
          .run(key, repoID == null ? null : String(repoID), slug, n, r.title ?? '', r.body ?? '', r.component ?? '',
               r.kind ?? 'tooling', Math.round(r.minutes ?? 0), r.possibleDuplicateOf ? JSON.stringify(r.possibleDuplicateOf) : null, at, at);
        report({ ...r, reporter: r.reporter?.login ?? null, reporterID: r.reporter?.githubID ?? null });
        break;
      }
      case 'counted': case 'regressed': {
        const row = this.sql.issue.get(key);
        if (!row) return;
        set({ count: row.count + 1, minutes: row.minutes + Math.round(r.minutes ?? 0),
              ...(kind === 'regressed' ? { state: 'regressed' } : {}), ...touched });
        report(r);
        break;
      }
      case 'triaged': {
        const row = this.sql.issue.get(key);
        if (!row) return;
        set({ kind: r.kind || row.kind, title: r.title || row.title, state: 'triaged', ...touched });
        break;
      }
      case 'fixed': set({ fixSha: r.sha ?? null, fixURL: r.url ?? null, fixRepoID: r.repoID ?? null, state: 'fixed', ...touched }); break;
      case 'wontfix': set({ reason: r.reason ?? null, state: 'wontfix', ...touched }); break;
      case 'closed': set({ state: 'closed', ...touched }); break;
      case 'workaround': set({ workaround: r.text ?? null, workaroundBy: r.by ?? null, ...touched }); break;
      case 'attached':
        db.prepare(`INSERT OR REPLACE INTO attachments (upload, id, name, type, kind, size, sha256, key, by, byID, scan)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
          .run(r.upload, key, r.name ?? null, r.type ?? null, r.kind ?? null, r.size ?? null, r.sha256 ?? null, r.key ?? null,
               r.by ?? null, r.byID ?? null, r.scan ? JSON.stringify(r.scan) : null);
        set(touched);
        break;
      case 'purged': set({ purged: 1, ...touched }); break;
      case 'deleted': set({ deleted: 1, deletedBy: r.by ?? null, deletedReason: r.reason ?? null, ...touched }); break;
      // an issue as its actor held it, written once by the migration to the
      // catalog: what the old ledger did not record (each report's minutes)
      case 'migrated': {
        const row = this.sql.issue.get(key);
        const fields = {
          title: r.title, body: r.body ?? '', component: r.component, kind: r.kind, state: r.state, count: r.count,
          minutes: Math.round(r.minutes ?? 0), workaround: r.workaround?.text ?? null, workaroundBy: r.workaround?.by ?? null,
          fixSha: r.fix?.sha ?? null, fixURL: r.fix?.url ?? null, fixRepoID: r.fix?.repoID ?? null, reason: r.reason ?? null,
          filed: r.filed ?? row?.filed ?? at, updated: r.updated ?? row?.updated ?? at,
          reporters: JSON.stringify(r.reporters ?? []), purged: r.purged ? 1 : 0,
          possible: r.possibleDuplicateOf ? JSON.stringify(r.possibleDuplicateOf) : row?.possible ?? null,
        };
        if (!row) {
          const repoID = r.repoID ?? this.sql.repoOf.get(slug)?.repoID ?? null;
          db.prepare('INSERT INTO issues (id, repoID, slug, n, title, component, kind, state) VALUES (?, ?, ?, ?, ?, ?, ?, ?)')
            .run(key, repoID == null ? null : String(repoID), slug, n, fields.title ?? '', fields.component ?? '', fields.kind ?? 'tooling', fields.state ?? 'new');
        }
        set(fields);
        for (const a of r.attachments ?? []) {
          db.prepare(`INSERT OR IGNORE INTO attachments (upload, id, name, type, kind, size, sha256, key, by, byID, scan)
                      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
            .run(a.upload, key, a.name ?? null, a.type ?? null, a.kind ?? null, a.size ?? null, a.sha256 ?? null, a.key ?? null,
                 a.by ?? null, a.byID ?? null, a.scan ? JSON.stringify(a.scan) : null);
        }
        break;
      }
      default: return;
    }
    this.#index(key);
  }

  #index(id) {
    this.sql.ftsDelete.run(id);
    const row = this.sql.issue.get(id);
    if (row && !row.deleted) this.sql.ftsInsert.run(id, row.title, row.body, row.workaround ?? '');
  }

  // The repositories, as the tracker's inventory names them (every name a
  // repository had reaches it, and its slug).
  projects(list) {
    this.#transaction(() => {
      const upsert = this.database.prepare(`INSERT INTO projects (repoID, slug, name, names, archived) VALUES (?, ?, ?, ?, ?)
        ON CONFLICT (repoID) DO UPDATE SET name = excluded.name, names = excluded.names, archived = excluded.archived`);
      for (const p of list) upsert.run(String(p.repoID), p.slug, p.name, JSON.stringify(p.names ?? [p.name]), p.archived ? 1 : 0);
      // an issue from before its records named the repository
      this.database.exec('UPDATE issues SET repoID = (SELECT repoID FROM projects WHERE projects.slug = issues.slug) WHERE repoID IS NULL');
    });
  }

  // --- the processor --------------------------------------------------------------

  processor = (event, data) => {
    switch (event) {
      case 'ledger.append': {
        if (this.failing > 0) { this.failing--; return null; }
        return { event: 'ledger.written', data: this.append(data.kind, data.key, data.record) };
      }
      case 'catalog.candidates': return { event: 'candidates', data: { issues: this.candidates(data) } };
      case 'catalog.project': return { event: 'project.loaded', data: this.project(data.repoID) };
      case 'catalog.projects': this.projects(data.projects ?? []); return { event: 'projects.noted', data: {} };
      case 'catalog.inventory': return { event: 'inventory', data: { repos: this.inventory() } };
    }
    return null;
  };

  // --- reading -----------------------------------------------------------------------

  get records() {
    return this.database.prepare('SELECT seq, at, kind, key, record FROM ledger ORDER BY seq').all()
      .map((r) => ({ ...r, record: JSON.parse(r.record) }));
  }

  // slug -> the repository's current name
  names() {
    return Object.fromEntries(this.database.prepare('SELECT slug, name FROM projects').all().map((p) => [p.slug, p.name]));
  }

  // The project an owner/name, a past name or a slug reaches, or null.
  resolve(repo) {
    for (const p of this.database.prepare('SELECT * FROM projects').all()) {
      if (p.slug === repo || p.name === repo || JSON.parse(p.names).includes(repo)) return p;
    }
    return null;
  }

  // The tracker's inventory: repoID -> {name, slug, archived, names}.
  inventory() {
    return Object.fromEntries(this.database.prepare('SELECT * FROM projects').all()
      .map((p) => [p.repoID, { name: p.name, slug: p.slug, archived: !!p.archived, names: JSON.parse(p.names) }]));
  }

  // Who reported an issue (their GitHub ids): from its reports, and, for
  // an issue from before the catalog, as its actor had them.
  #reporters(row) {
    const ids = this.database.prepare('SELECT DISTINCT reporterID FROM reports WHERE id = ? AND reporterID IS NOT NULL ORDER BY seq').all(row.id)
      .map((r) => r.reporterID);
    return [...new Set([...JSON.parse(row.reporters ?? '[]'), ...ids])];
  }

  validNames() {
    return this.database.prepare('SELECT name FROM projects WHERE archived = 0 ORDER BY name').all().map((p) => p.name);
  }

  #summary(row) {
    return {
      id: row.id, n: row.n, title: row.title, excerpt: String(row.body ?? '').slice(0, 280), component: row.component,
      kind: row.kind, state: row.state, count: row.count, minutes: row.minutes, workaround: row.workaround ?? null,
      fix: row.fixSha ?? null, possibleDuplicateOf: row.possible ? JSON.parse(row.possible) : null,
      attachments: this.database.prepare('SELECT count(*) AS n FROM attachments WHERE id = ?').get(row.id).n,
      filed: row.filed ?? null, updated: row.updated ?? null,
    };
  }

  // The issues known_issues lists: by repository (a name or a slug),
  // component, state (open, all, or one) and words (FTS5, best match
  // first; else the costliest first). Archived repositories only with all.
  list({ repo, component, state = 'open', query } = {}) {
    const where = ['i.deleted = 0'], params = [];
    if (repo) {
      const project = this.resolve(repo);
      if (!project) return [];
      where.push('i.slug = ?'); params.push(project.slug);
    }
    if (state !== 'all') where.push('(p.archived IS NULL OR p.archived = 0)');
    if (component) { where.push('i.component = ?'); params.push(component); }
    if (state === 'open') where.push(`i.state IN (${OPEN.map(() => '?').join(', ')})`), params.push(...OPEN);
    else if (STATES.includes(state)) where.push('i.state = ?'), params.push(state);
    let from = 'issues i LEFT JOIN projects p ON p.slug = i.slug', order = 'i.count * i.minutes DESC, i.slug, i.n';
    const words = terms(query);
    if (query !== undefined && query !== null && String(query).trim() !== '') {
      if (!words.length) return [];
      from = `issues_fts f JOIN issues i ON i.id = f.id LEFT JOIN projects p ON p.slug = i.slug`;
      where.push('issues_fts MATCH ?'); params.push(words.join(' '));
      order = 'bm25(issues_fts), i.n';
    }
    return this.database.prepare(`SELECT i.* FROM ${from} WHERE ${where.join(' AND ')} ORDER BY ${order}`).all(...params)
      .map((row) => this.#summary(row));
  }

  // One issue in full, for get_issue: its reports and its files too; or
  // {gone} for a deleted one, null for none.
  get(slug, n) {
    const row = this.database.prepare('SELECT * FROM issues WHERE slug = ? AND n = ?').get(slug, n);
    if (!row) return null;
    if (row.deleted) return { gone: true, id: row.id };
    const reports = this.database.prepare('SELECT * FROM reports WHERE id = ? ORDER BY seq').all(row.id);
    const files = this.database.prepare('SELECT * FROM attachments WHERE id = ? ORDER BY upload').all(row.id);
    return {
      id: row.id, repoID: row.repoID, n: row.n, title: row.title, state: row.state, component: row.component, kind: row.kind,
      body: row.body, count: row.count, minutes: row.minutes,
      workaround: row.workaround ? { text: row.workaround, by: row.workaroundBy } : null,
      fix: row.fixSha ? { sha: row.fixSha, url: row.fixURL, repoID: row.fixRepoID } : null,
      reason: row.reason ?? null, filed: row.filed ?? null, updated: row.updated ?? null, purged: !!row.purged,
      occurrences: reports.slice(-50).map((r) => ({ session: r.session, commit: r.commit, at: r.at, reporter: r.reporter })),
      reporterIDs: this.#reporters(row),
      attachments: files.map((a) => ({ upload: a.upload, name: a.name, type: a.type, kind: a.kind, size: a.size, sha256: a.sha256,
                                       key: a.key, by: a.by, byID: a.byID, scan: a.scan ? JSON.parse(a.scan) : null })),
    };
  }

  workaround(id) {
    const row = this.sql.issue.get(id);
    return row?.workaround ? { text: row.workaround, by: row.workaroundBy } : null;
  }

  attachment(upload) {
    const a = this.database.prepare('SELECT * FROM attachments WHERE upload = ?').get(upload);
    return a ? { upload: a.upload, name: a.name, type: a.type, kind: a.kind, size: a.size, sha256: a.sha256, key: a.key,
                 by: a.by, byID: a.byID, scan: a.scan ? JSON.parse(a.scan) : null } : null;
  }

  // The open issues of a repository a report could be (the decision
  // model's candidates): FTS5 over the report's words, best first by bm25,
  // an issue of the report's component counting twice.
  candidates({ repoID, component, title, body, limit = 20 }) {
    const words = terms(`${title ?? ''} ${body ?? ''}`);
    if (!words.length) return [];
    return this.database.prepare(`
      SELECT i.n, i.title, i.component, i.kind, i.state, substr(i.body, 1, 280) AS excerpt
      FROM issues_fts f JOIN issues i ON i.id = f.id
      WHERE issues_fts MATCH ? AND i.repoID = ? AND i.deleted = 0 AND i.state IN (${OPEN.map(() => '?').join(', ')})
      ORDER BY bm25(issues_fts) * (CASE WHEN i.component = ? THEN 2 ELSE 1 END), i.n
      LIMIT ?`).all(words.join(' OR '), String(repoID), ...OPEN, component ?? '', limit);
  }

  // What a project starts from: its next number and every issue's subset
  // (its actor's, for a rebuild; the project's fingerprints and index).
  project(repoID) {
    const rows = this.database.prepare('SELECT * FROM issues WHERE repoID = ? ORDER BY n').all(String(repoID));
    const next = rows.reduce((max, r) => Math.max(max, r.n + 1), 1);
    return {
      next,
      issues: rows.map((r) => {
        const files = this.database.prepare('SELECT upload, key FROM attachments WHERE id = ?').all(r.id);
        return {
          n: r.n, title: r.title, component: r.component, state: r.state, deleted: !!r.deleted, count: r.count,
          reporters: this.#reporters(r), reason: r.reason ?? null, purged: !!r.purged,
          fix: r.fixSha ? { sha: r.fixSha, url: r.fixURL, repoID: r.fixRepoID } : null,
          files: Object.fromEntries(files.map((f) => [f.upload, f.key])),
        };
      }),
    };
  }

  // --- recovery and migration ---------------------------------------------------------

  // The catalog's rows again, from the ledger: every change replayed.
  rebuild() {
    this.#transaction(() => {
      this.database.exec('DELETE FROM issues; DELETE FROM reports; DELETE FROM attachments; DELETE FROM issues_fts;');
      for (const row of this.records) this.#apply(row);
      this.database.exec('UPDATE issues SET repoID = (SELECT repoID FROM projects WHERE projects.slug = issues.slug) WHERE repoID IS NULL');
    });
    return this.database.prepare('SELECT count(*) AS n FROM issues').get().n;
  }

  issueCount() { return this.database.prepare('SELECT count(*) AS n FROM issues').get().n; }
  meta(name) { return this.database.prepare('SELECT value FROM meta WHERE name = ?').get(name)?.value ?? null; }

  // From the ledger.sqlite of before the catalog: its rows, in order, with
  // their times, replayed; then one `migrated` record per issue as its
  // actor held it (`issues`: the old actors' variables, read before they
  // move to the new charts), for what the old ledger did not record (each
  // report's minutes and reporter, the files). Once: a second call does
  // nothing.
  migrate({ legacy, issues = [] }) {
    if (this.meta('migrated')) return false;
    this.#transaction(() => {
      if (legacy && existsSync(legacy)) {
        const old = new DatabaseSync(legacy, { readOnly: true });
        for (const r of old.prepare('SELECT at, kind, key, record FROM ledger ORDER BY seq').all()) {
          const { lastInsertRowid } = this.sql.append.run(r.at, r.kind, r.key, r.record);
          this.#apply({ seq: Number(lastInsertRowid), at: r.at, kind: r.kind, key: r.key, record: JSON.parse(r.record) ?? {} });
        }
        old.close();
      }
      const at = new Date(this.now()).toISOString();
      for (const issue of issues) {
        const record = { ...issue };
        delete record.occurrences;
        const { lastInsertRowid } = this.sql.append.run(at, 'migrated', issue.id, JSON.stringify(record));
        this.#apply({ seq: Number(lastInsertRowid), at, kind: 'migrated', key: issue.id, record });
      }
      // an issue the old ledger filed and no actor holds was deleted (its
      // delete not recorded then): recorded as deleted now
      if (issues.length) {
        const held = new Set(issues.map((i) => i.id));
        for (const { id } of this.database.prepare('SELECT id FROM issues WHERE deleted = 0').all()) {
          if (held.has(id)) continue;
          const record = { by: null, reason: 'no actor held it when the catalog was made' };
          const { lastInsertRowid } = this.sql.append.run(at, 'deleted', id, JSON.stringify(record));
          this.#apply({ seq: Number(lastInsertRowid), at, kind: 'deleted', key: id, record });
        }
      }
      this.database.prepare('INSERT INTO meta (name, value) VALUES (?, ?)').run('migrated', at);
    });
    return true;
  }
}
