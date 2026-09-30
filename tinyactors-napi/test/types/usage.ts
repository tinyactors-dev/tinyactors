// A consumer's view of index.d.ts, type-checked by `mise run check-types`
// against Node's types and ES2022 only: no DOM. It is compiled, never run.

import { createSystem, named, parameterInfo, readInputLog, serialize, statechart } from '@tinyactors/node';
import type { ActorAddress, ActorEvent, InputRecord, ReplayReport } from '@tinyactors/node';

class Money {
  constructor(readonly amount: number, readonly currency: string) {}
  [serialize]() { return { name: 'Money', arguments: [this.amount, this.currency] }; }
}

export async function usage(): Promise<void> {
  // A system is disposable (explicit resource management), created with its parameters.
  using system = createSystem({ parameters: { valueBytes: 16384, microstepLimit: 50_000 } });
  const bytes16k: number = system.parameters.valueBytes;
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

// Typed event maps: statechart<Data, Events>() checks on(), raise() and
// send() against the events the chart handles, and a transition's
// expressions see the data of the events its descriptor matches.
interface OrderEvents {
  'order.placed': { readonly id: string; readonly total: number };
  'order.cancelled': { readonly id: string };
  tick: undefined;
  pong: { readonly from: string };
}

export function typedEvents(): void {
  statechart<{ total: number; last: string }, OrderEvents>({ family: 'orders', revision: '1' })
    .data('total', 0)
    .data('last', '')
    .state('open', (s) => s
      // One event: its data, and its name.
      .on('order.placed', (t) => t
        .assign('total', function ({ event }) { return this.total + (event?.data.total ?? 0); })
        .assign('last', ({ event }) => {
          const name: 'order.placed' | undefined = event?.name;
          return name ?? '';
        })
        .when(({ event }) => event !== undefined && event.data.total > 0))
      // A prefix matches both order events; the name tells them apart.
      .on('order', (t) => t.assign('last', ({ event }) => {
        if (event?.name === 'order.cancelled') {
          const id: string = event.data.id;
          return id;
        }
        return event?.data.id ?? '';
      }))
      .on('order.*', (t) => t.assign('last', ({ event }) => event?.data.id ?? ''))
      // A list: the union of its events' data.
      .on('tick pong', (t) => t.assign('last', ({ event }) => (event?.name === 'pong' ? event.data.from : 'tick')))
      // Platform events and the wildcard are always accepted, with unknown data.
      .on('done.invoke.worker', (t) => t.assign('last', ({ event }) => {
        const data: unknown = event?.data;
        void data;
        return 'done';
      }))
      .on('error.*')
      .on('error.execution')
      .on('*', (t) => t.target('open'))
      // raise(): a name of the map, with that event's data.
      .on('tick', (t) => t
        .raise('tick')
        .raise('order.cancelled', { id: 'o1' })
        .raise('order.placed', ({ data }) => ({ id: 'o2', total: data.total })))
      // send(): data() is checked for a name of the map; other names go elsewhere.
      .on('pong', (t) => t
        .send('order.cancelled', (send) => send.data({ id: 'o3' }))
        .send('ping', (send) => send.to(({ event }) => event?.data.from ?? '').data({ anything: true }))
        .send(({ event }) => `echo.${event?.name ?? ''}`)
        // A reply to the sender, through its I/O processor.
        .send('pong', (send) => send.toOrigin().data({ from: 'orders' }))));

  statechart<{ n: number }, OrderEvents>({ family: 'orders-bad', revision: '1' })
    .state('s', (s) => s
      // @ts-expect-error no event of the map is called "order.shipped"
      .on('order.shipped')
      // @ts-expect-error a typo in one item of a list
      .on('tick pomg')
      // @ts-expect-error "ord" is not a prefix on a "." boundary
      .on('ord')
      // @ts-expect-error order.placed's data has no field "count"
      .on('order.placed', (t) => t.assign('n', ({ event }) => event?.data.count))
      // @ts-expect-error tick's data is undefined, so "from" is not there
      .on('tick', (t) => t.assign('n', ({ event }) => event?.data.from.length))
      .on('tick', (t) => t
        // @ts-expect-error raise() takes a name of the map
        .raise('tock')
        // @ts-expect-error order.cancelled needs its data
        .raise('order.cancelled')
        // @ts-expect-error order.cancelled's data is { id: string }
        .raise('order.cancelled', { id: 1 })
        // @ts-expect-error a send of an event of the map checks its data
        .send('order.placed', (send) => send.data({ id: 'o' }))));

  // Without an event map, every name is accepted and data is unknown, as before.
  statechart<{ n: number }>({ family: 'untyped', revision: '1' })
    .state('s', (s) => s
      .on('anything.at.all', (t) => t
        .raise('whatever', { any: 'data' })
        .send('other', (send) => send.data(42))
        .assign('n', ({ event }) => {
          const seen: ActorEvent | undefined = event;
          const data: unknown = seen?.data;
          return typeof data === 'number' ? data : 0;
        }))
      .on('a b.* *'));
}
