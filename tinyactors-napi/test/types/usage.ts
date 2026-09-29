// A consumer's view of index.d.ts, type-checked by `mise run check-types`
// against Node's types and ES2022 only: no DOM. It is compiled, never run.

import { createSystem, named, parameterInfo, readInputLog, serialize, statechart } from '@tinyactors/node';
import type { ActorAddress, InputRecord, ReplayReport } from '@tinyactors/node';

class Money {
  constructor(readonly amount: number, readonly currency: string) {}
  [serialize]() { return { name: 'Money', arguments: [this.amount, this.currency] }; }
}

export async function usage(): Promise<void> {
  // A system is disposable (explicit resource management), created with its parameters.
  using system = createSystem({ parameters: { traceValueBytes: 16384, microstepLimit: 50_000 } });
  const bytes16k: number = system.parameters.traceValueBytes;
  const bound: number = parameterInfo().interpreterBytes.max;
  void bytes16k; void bound;
  // A function charts call by name, so the chart is content core sees whole.
  system.registerFunction('full', function (this: { count: number }) { return this.count >= 10; });
  const counter = system.define(
    statechart<{ count: number }>({ family: 'counter', revision: '1' })
      .data('count', 0)
      .state('counting', (state) => state
        .on('add', (t) => t.assign('count', function () { return this.count + 1; }))
        .on('check', (t) => t.when(named('full')).target('counting'))),
  );
  using actor = system.spawn(counter);

  // A class charts build by name, and a value that describes itself for text.
  system.registerClass('Money', Money);
  const priced = system.define(statechart<{ price: Money }>({ family: 'priced', revision: '1' })
    .data('price', new Money(12, 'EUR')).state('open'));
  const { bytes } = priced.encode('scxml', { datamodel: 'javascript' });
  void bytes;

  // A caller cancels a send with Node's AbortController; the API takes its signal.
  const controller = new AbortController();
  actor.post('add', undefined, { signal: controller.signal });
  await actor.send('add', undefined, { signal: controller.signal });
  controller.abort();

  // Persistence by replay (napi ADRs 0032 to 0034): a write-ahead log of
  // opaque bytes the application stores without parsing, a loader that
  // answers { replay }, wakes, eviction, checkpoints, an upgrade by replay,
  // and an audit read.
  const log = new Map<string, Uint8Array[]>();
  const wakes = new Map<string, number>();
  const key = (address: ActorAddress) => `${address.family}/${address.name}`;
  system.setInputLog({
    commit({ entries, wakes: changed }) {
      for (const { address, bytes } of entries) log.set(key(address), [...(log.get(key(address)) ?? []), bytes]);
      for (const { address, at } of changed) at === null ? wakes.delete(key(address)) : wakes.set(key(address), at);
    },
  }, { families: ['counter'] });
  system.setLoader('counter', (target) => ('name' in target
    ? { replay: { spawn: counter, inputs: log.get(key(target)) ?? [], onReport: (report) => void report.ignored } }
    : 'not-found'));
  const checkpoint: Uint8Array[] = [];
  system.checkpoint(actor, (chunk) => checkpoint.push(chunk));
  const evictedAt: number = system.evict(actor);
  void evictedAt;
  system.wakeAt({ family: 'counter', name: 'a' }, wakes.get('counter/a') ?? 0);
  const history = log.get('counter/again') ?? [];
  const prepared = system.prepare(counter, { address: { family: 'counter', name: 'again' }, log: history[0] ?? new Uint8Array() });
  const report: ReplayReport = await prepared.replay(history.slice(1));
  system.publish(prepared);
  for await (const record of readInputLog(history)) {
    const kind: InputRecord['kind'] = record.kind;
    void kind;
  }
  void report.inputs;
}
