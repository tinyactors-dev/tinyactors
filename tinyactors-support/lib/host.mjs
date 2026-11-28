// The host: the charts on @tinyactors/node, the I/O processors, the store
// of hibernated actors and the snapshots that carry it all over a restart.
//
// The actors' durability is the snapshot and the store together. A blob is
// deleted only once a snapshot that no longer needs it is on disk
// (DeferredStore), and a snapshot is taken shortly after each change and
// at shutdown, so a crash loses at most the last seconds of the actors'
// state. The issues themselves are the catalog's (catalog.sqlite), written
// before each answer: without a snapshot (`rebuild`: lost, or unreadable)
// the host starts empty and the tracker and the projects spawn every
// project and issue again from it; logins, users and grants are not in
// it, so people sign in again.

import { existsSync, readFileSync, renameSync, writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { Host, SQLiteStore, Spawner, StaleError, StateError } from '@tinyactors/node';
import { compile } from './charts.mjs';
import { processor } from './processors/index.mjs';

export const SYSTEM = 'support';

// Deletes wait for the next snapshot: a crash restores the last snapshot,
// which may still name an actor as hibernated whose blob a wake deleted.
class DeferredStore {
  constructor(store) { this.store = store; this.deleted = new Set(); }
  save(session, blob) { this.deleted.delete(session); this.store.save(session, blob); }
  load(session) { return this.store.load(session); }
  delete(session) { this.deleted.add(session); }
  flush() { for (const session of this.deleted) this.store.delete(session); this.deleted.clear(); }
  close() { this.store.close?.(); }
}

export class Support {
  // processors: {github, catalog, decide, blobs, scan}, each `(event, data) => answer`
  // data: a directory for the snapshot and the store, or null (in memory, tests)
  // rebuild: start from the catalog, the snapshot and the store set aside
  // (renamed with the time: never deleted)
  constructor({ processors, data = null, clock, resident = 1000, log = () => {}, snapshotDelay = 2000, rebuild = false }) {
    this.log = log;
    this.data = data;
    this.snapshotDelay = snapshotDelay;
    this.clock = clock;
    if (data) mkdirSync(data, { recursive: true });
    if (data && rebuild) {
      const stamp = new Date().toISOString().replace(/[:.]/g, '-');
      for (const file of ['host.snapshot', 'store.sqlite', 'store.sqlite-wal', 'store.sqlite-shm']) {
        if (existsSync(join(data, file))) renameSync(join(data, file), join(data, `${file}.before-rebuild-${stamp}`));
      }
      log(`rebuild: the snapshot and the store set aside (.before-rebuild-${stamp}); the actors start from the catalog`);
    }
    this.store = new DeferredStore(new SQLiteStore(data ? join(data, 'store.sqlite') : ':memory:'));
    this.compiled = compile();
    const options = {
      system: SYSTEM, clock, store: this.store, resident,
      ioProcessors: {
        spawn: new Spawner(),
        ...Object.fromEntries(Object.entries(processors).map(([type, handle]) => [type, processor(handle)])),
        // the catalog's name before it was one: for actors on charts of before
        ...(processors.catalog && !processors.ledger ? { ledger: processor(processors.catalog) } : {}),
      },
      log: (entry) => log(`[${entry.label || 'chart'}] ${entry.text}`),
      onDone: (actor, donedata) => this.onDone?.(actor, donedata),
      onError: (actor, error) => log(`an actor faulted: ${error.message}`),
      onHandlerError: (error) => log(`a handler failed: ${error?.stack ?? error}`),
    };
    const snapshotPath = data && join(data, 'host.snapshot');
    if (snapshotPath && existsSync(snapshotPath)) {
      this.host = Host.restore(readFileSync(snapshotPath), this.compiled, options);
      this.restored = true;
      log(`restored from ${snapshotPath}`);
    } else {
      this.host = new Host(this.compiled, options);
      this.host.spawn('tracker', { name: 'tracker' });
      this.host.spawn('auth', { name: 'auth' });
      this.restored = false;
    }
    this.snapshotPath = snapshotPath;
    this.timer = null;
    // What actors do on their own (a pull from GitHub, a delayed send) is
    // a change too: a snapshot every five minutes covers it.
    this.periodic = snapshotPath ? setInterval(() => this.changed(), 300_000) : null;
    this.periodic?.unref?.();
  }

  // Charts deployed since the snapshot are a new version: every actor moves
  // to it. A step of its own, so the server can read the old actors first.
  upgrade() {
    if (!this.restored) return;
    try { this.host.migrate({ wake: true }); } catch (error) { this.log(`no migration: ${error.message}`); }
  }

  now() { return this.host.now(); }

  // The time of day for what the charts record (they read no clock): the
  // simulated clock's, from 2026-01-01, under a test; else the machine's.
  wallNow() { return this.clock ? Date.UTC(2026, 0, 1) + this.clock.now : Date.now(); }

  // An event for a named actor, answered to the ask: [event, data], or
  // {event: 'stale'} when no actor has the name, {event: 'timeout'}.
  async request(name, event, data, timeout = 10_000) {
    let actor;
    try { actor = this.host.actor(name); } catch (error) {
      if (error instanceof StaleError) return { event: 'stale' };
      throw error;
    }
    try {
      const [answered, value] = await actor.ask(event, data, { signal: AbortSignal.timeout(timeout) });
      this.changed();
      return { event: answered, data: value };
    } catch (error) {
      if (error instanceof StaleError) return { event: 'stale' };
      if (error?.name === 'TimeoutError' || error?.name === 'AbortError') return { event: 'timeout' };
      throw error;
    }
  }

  // An event nobody answers.
  post(name, event, data) {
    try { this.host.actor(name).send(event, data); this.changed(); return true; }
    catch (error) { if (error instanceof StaleError) return false; throw error; }
  }

  // A snapshot shortly after a change (one at a time, when the host is quiescent).
  changed() {
    if (!this.snapshotPath || this.timer) return;
    this.timer = setTimeout(() => { this.timer = null; this.snapshot().catch((e) => this.log(`snapshot: ${e.message}`)); }, this.snapshotDelay);
    this.timer.unref?.();
  }

  async snapshot() {
    if (!this.snapshotPath) return null;
    for (let attempt = 0; attempt < 20; attempt++) {
      await this.host.idle();
      try {
        const bytes = this.host.snapshot();
        writeFileSync(`${this.snapshotPath}.partial`, bytes);
        renameSync(`${this.snapshotPath}.partial`, this.snapshotPath);
        this.store.flush();
        return bytes.length;
      } catch (error) {
        if (!(error instanceof StateError)) throw error;   // a reply or a migration in flight: a moment later
        await new Promise((r) => setTimeout(r, 50));
      }
    }
    throw new Error('the host did not become quiescent for a snapshot');
  }

  async close() {
    clearTimeout(this.timer);
    clearInterval(this.periodic);
    if (this.snapshotPath) await this.snapshot();
    this.host.close();
    this.store.close();
  }
}
