// The host: the charts on @tinyactors/node, the I/O processors, the store
// of hibernated actors and the snapshots that carry it all over a restart.
//
// Durability is the snapshot and the store together (the ledger is the
// journal). A blob is deleted only once a snapshot that no longer needs it
// is on disk (DeferredStore), and a snapshot is taken shortly after each
// change and at shutdown, so a crash loses at most the last seconds.

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
  // processors: {github, ledger, decide, blobs, scan}, each `(event, data) => answer`
  // data: a directory for the snapshot and the store, or null (in memory, tests)
  constructor({ processors, data = null, clock, resident = 1000, log = () => {}, snapshotDelay = 2000 }) {
    this.log = log;
    this.data = data;
    this.snapshotDelay = snapshotDelay;
    this.clock = clock;
    if (data) mkdirSync(data, { recursive: true });
    this.store = new DeferredStore(new SQLiteStore(data ? join(data, 'store.sqlite') : ':memory:'));
    this.compiled = compile();
    const options = {
      system: SYSTEM, clock, store: this.store, resident,
      ioProcessors: {
        spawn: new Spawner(),
        ...Object.fromEntries(Object.entries(processors).map(([type, handle]) => [type, processor(handle)])),
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
      // charts deployed since the snapshot are a new version: move every actor to it
      try { this.host.migrate({ wake: true }); } catch (error) { log(`no migration: ${error.message}`); }
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
