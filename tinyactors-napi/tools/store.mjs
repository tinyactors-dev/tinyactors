// What the gates share (tools/stress.mjs, tools/migration.mjs; as
// tinyactors-cpython/tools/stress.py shares its Store with migration.py):
// the batch size, a seeded generator and the store of hibernated actors'
// blobs, each runtime's own SQLite.

export const BATCH = 256;

// A seeded generator (mulberry32), so a run is the same run again.
export function random(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// The hibernated actors' blobs: one SQLite row per session, written in
// transactions of 4096.
export class Store {
  static async open(file) {
    if (globalThis.Bun) {
      const { Database } = await import('bun:sqlite');
      const db = new Database(file);
      return new Store({ exec: (sql) => db.run(sql), prepare: (sql) => db.prepare(sql) });
    }
    const { DatabaseSync } = await import('node:sqlite');
    const db = new DatabaseSync(file);
    return new Store({ exec: (sql) => db.exec(sql), prepare: (sql) => db.prepare(sql) });
  }

  constructor(db) {
    this.db = db;
    db.exec('PRAGMA journal_mode=WAL');
    db.exec('PRAGMA synchronous=OFF');
    db.exec('CREATE TABLE blobs (session INTEGER PRIMARY KEY, blob BLOB NOT NULL)');
    this.insert = db.prepare('INSERT OR REPLACE INTO blobs VALUES (?, ?)');
    this.select = db.prepare('SELECT blob FROM blobs WHERE session = ?');
    this.pending = [];
    this.puts = 0;
    this.gets = 0;
  }

  put(session, blob) {
    this.pending.push([session, blob]);
    this.puts++;
    if (this.pending.length >= 4096) this.flush();
  }

  flush() {
    if (this.pending.length === 0) return;
    this.db.exec('BEGIN');
    for (const [session, blob] of this.pending) this.insert.run(session, blob);
    this.db.exec('COMMIT');
    this.pending.length = 0;
  }

  get(session) {
    this.flush(); // a wake can come before a put settled: read your own writes
    this.gets++;
    return this.select.get(session).blob;
  }
}
