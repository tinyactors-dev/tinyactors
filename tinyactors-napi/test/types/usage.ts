// A consumer's view of index.d.ts, type-checked by `mise run check-types`
// against Node's types and ES2022 only: no DOM. It is compiled, never run.

import { createSystem, named, serialize, statechart } from '@tinyactors/node';

class Money {
  constructor(readonly amount: number, readonly currency: string) {}
  [serialize]() { return { name: 'Money', arguments: [this.amount, this.currency] }; }
}

export async function usage(): Promise<void> {
  // A system is disposable (explicit resource management).
  using system = createSystem();
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
}
