PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS tasks (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('planned','active','review','revision','accepted','blocked')),
  owner TEXT NOT NULL DEFAULT 'root',
  context TEXT NOT NULL,
  acceptance TEXT NOT NULL,
  result TEXT,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
CREATE TABLE IF NOT EXISTS dependencies (
  task_id TEXT NOT NULL REFERENCES tasks(id),
  prerequisite_id TEXT NOT NULL REFERENCES tasks(id),
  PRIMARY KEY(task_id, prerequisite_id)
);
CREATE TABLE IF NOT EXISTS iterations (
  id INTEGER PRIMARY KEY,
  task_id TEXT NOT NULL REFERENCES tasks(id),
  agent TEXT NOT NULL,
  instruction TEXT NOT NULL,
  outcome TEXT,
  review TEXT,
  started_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  completed_at TEXT
);
CREATE TABLE IF NOT EXISTS checks (
  id INTEGER PRIMARY KEY,
  task_id TEXT NOT NULL REFERENCES tasks(id),
  iteration_id INTEGER REFERENCES iterations(id),
  command TEXT NOT NULL,
  exit_code INTEGER,
  output TEXT NOT NULL,
  checked_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
CREATE TABLE IF NOT EXISTS artifacts (
  task_id TEXT NOT NULL REFERENCES tasks(id),
  path TEXT NOT NULL,
  description TEXT NOT NULL,
  PRIMARY KEY(task_id,path)
);
CREATE TABLE IF NOT EXISTS decisions (
  id INTEGER PRIMARY KEY,
  task_id TEXT REFERENCES tasks(id),
  context TEXT NOT NULL,
  decision TEXT NOT NULL,
  recorded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
