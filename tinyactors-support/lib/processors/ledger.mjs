// The journal of every change the charts make: issues filed, counted,
// triaged, fixed, attachments, decisions. Written before an issue answers
// (the chart waits for ledger.written). It is the audit and the export,
// not the truth: the actors are, through the snapshot and the store.

import { DatabaseSync } from 'node:sqlite';

export class Ledger {
  constructor(path = ':memory:') {
    this.database = new DatabaseSync(path);
    this.database.exec(`CREATE TABLE IF NOT EXISTS ledger (seq INTEGER PRIMARY KEY, at TEXT NOT NULL,
      kind TEXT NOT NULL, key TEXT NOT NULL, record TEXT NOT NULL)`);
    this.insert = this.database.prepare('INSERT INTO ledger (at, kind, key, record) VALUES (?, ?, ?, ?)');
    this.failing = 0;
  }
  get records() {
    return this.database.prepare('SELECT at, kind, key, record FROM ledger ORDER BY seq').all()
      .map((r) => ({ ...r, record: JSON.parse(r.record) }));
  }
  processor = (event, data) => {
    if (event !== 'ledger.append') return null;
    if (this.failing > 0) { this.failing--; return null; }
    const { lastInsertRowid } = this.insert.run(new Date().toISOString(), data.kind, data.key, JSON.stringify(data.record ?? null));
    return { event: 'ledger.written', data: { at: Number(lastInsertRowid) } };
  };
  close() { this.database.close(); }
}
