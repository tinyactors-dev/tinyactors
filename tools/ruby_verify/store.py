"""SQLite evidence. Only the harness writes verdicts; attempts are never discarded."""
import contextlib
import datetime as dt
import json
import sqlite3


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


SCHEMA = """
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS check_versions(
  check_id TEXT NOT NULL, revision TEXT NOT NULL, spec_json TEXT NOT NULL,
  created_at TEXT NOT NULL, PRIMARY KEY(check_id, revision));
CREATE TABLE IF NOT EXISTS checks(
  check_id TEXT PRIMARY KEY, revision TEXT NOT NULL, title TEXT NOT NULL,
  brief TEXT NOT NULL, kind TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1);
CREATE TABLE IF NOT EXISTS runs(
  run_id TEXT PRIMARY KEY, started_at TEXT NOT NULL, finished_at TEXT,
  status TEXT NOT NULL CHECK(status IN ('running','completed','interrupted')),
  agent TEXT NOT NULL, model TEXT, reasoning TEXT, task TEXT, iteration TEXT,
  context_json TEXT NOT NULL, scope_json TEXT NOT NULL, source_hash TEXT NOT NULL,
  catalog_hash TEXT NOT NULL, environment_hash TEXT NOT NULL,
  environment_json TEXT NOT NULL, git_head TEXT, git_status TEXT,
  source_manifest_json TEXT NOT NULL, artifact_dir TEXT NOT NULL,
  pid INTEGER NOT NULL, exit_code INTEGER);
CREATE TABLE IF NOT EXISTS results(
  run_id TEXT NOT NULL REFERENCES runs(run_id), check_id TEXT NOT NULL,
  revision TEXT NOT NULL, status TEXT NOT NULL
    CHECK(status IN ('pending','running','pass','fail','error')),
  started_at TEXT, finished_at TEXT, duration_ms INTEGER,
  assertions INTEGER NOT NULL DEFAULT 0, exit_code INTEGER,
  message TEXT, details_json TEXT NOT NULL DEFAULT '{}',
  stdout TEXT, stderr TEXT, command_json TEXT, log_prefix TEXT,
  PRIMARY KEY(run_id, check_id),
  FOREIGN KEY(check_id, revision) REFERENCES check_versions(check_id, revision));
CREATE INDEX IF NOT EXISTS results_by_check ON results(check_id, finished_at DESC);
CREATE INDEX IF NOT EXISTS runs_by_source ON runs(source_hash, catalog_hash, environment_hash);
CREATE VIEW IF NOT EXISTS check_history AS
SELECT r.check_id, r.revision, r.status, r.message, r.assertions, r.duration_ms,
       r.finished_at, r.run_id, n.agent, n.model, n.reasoning, n.task, n.iteration,
       n.context_json, n.source_hash, n.catalog_hash, n.environment_hash,
       r.stdout, r.stderr, r.log_prefix
FROM results r JOIN runs n USING(run_id);
"""


class Store:
    def __init__(self, path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, timeout=30, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("PRAGMA busy_timeout=30000")
        version = self.db.execute("PRAGMA user_version").fetchone()[0]
        if version not in (0, 1):
            raise RuntimeError(f"Unsupported verification DB version: {version}")
        self.db.executescript(SCHEMA)
        self.db.execute("PRAGMA user_version=1")

    def close(self):
        self.db.close()

    @contextlib.contextmanager
    def transaction(self):
        self.db.execute("BEGIN IMMEDIATE")
        try:
            yield
        except BaseException:
            self.db.execute("ROLLBACK")
            raise
        else:
            self.db.execute("COMMIT")

    def setting(self, key, default=None):
        row = self.db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        return row[0] if row else default

    def set_setting(self, key, value):
        self.db.execute("INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))

    def sync(self, checks):
        ids = {c["id"] for c in checks}
        old = {r[0] for r in self.db.execute("SELECT check_id FROM checks WHERE active=1")}
        removed = old - ids
        if removed:
            raise RuntimeError("Catalog removed required checks; restore their stable IDs: " + ", ".join(sorted(removed)))
        with self.transaction():
            for c in checks:
                self.db.execute("INSERT OR IGNORE INTO check_versions VALUES(?,?,?,?)", (c["id"], c["revision"], json.dumps(c, sort_keys=True), now()))
                self.db.execute("""INSERT INTO checks(check_id,revision,title,brief,kind) VALUES(?,?,?,?,?)
                    ON CONFLICT(check_id) DO UPDATE SET revision=excluded.revision,
                    title=excluded.title,brief=excluded.brief,kind=excluded.kind""",
                    (c["id"], c["revision"], c["title"], c["brief"], c["kind"]))

    def begin_run(self, run, checks):
        with self.transaction():
            keys = list(run)
            self.db.execute(f"INSERT INTO runs({','.join(keys)}) VALUES({','.join('?' for _ in keys)})", [run[k] for k in keys])
            self.db.executemany("INSERT INTO results(run_id,check_id,revision,status) VALUES(?,?,?,'pending')",
                                [(run["run_id"], c["id"], c["revision"]) for c in checks])

    def record(self, run_id, check_id, result):
        keys = list(result)
        with self.transaction():
            self.db.execute(f"UPDATE results SET {','.join(k+'=?' for k in keys)} WHERE run_id=? AND check_id=?",
                            [result[k] for k in keys] + [run_id, check_id])

    def finish(self, run_id, status, exit_code):
        with self.transaction():
            # A crash, failed setup, or interrupted worker cannot leave a check silently uncounted.
            self.db.execute("""UPDATE results SET status='error',finished_at=?,message='Run interrupted before check completed'
                WHERE run_id=? AND status IN ('pending','running')""", (now(), run_id))
            self.db.execute("UPDATE runs SET status=?,finished_at=?,exit_code=? WHERE run_id=?", (status, now(), exit_code, run_id))

    def latest(self, check_id):
        row = self.db.execute("""SELECT r.*, n.agent,n.model,n.reasoning,n.task,n.iteration,n.context_json,
                n.source_hash,n.catalog_hash,n.environment_hash,n.status AS run_status
            FROM results r JOIN runs n USING(run_id) WHERE r.check_id=?
            ORDER BY n.started_at DESC,n.run_id DESC LIMIT 1""", (check_id,)).fetchone()
        return dict(row) if row else None

    def history(self, check_id, limit=20):
        return [dict(r) for r in self.db.execute("SELECT * FROM check_history WHERE check_id=? ORDER BY finished_at DESC LIMIT ?", (check_id, limit))]
