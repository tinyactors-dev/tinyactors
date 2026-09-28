// A consumer's view of index.d.ts, type-checked by `mise run check-types`
// against Node's types and ES2022 only: no DOM. It is compiled, never run.

import { createSystem, statechart } from '@tinyactors/node';

export async function usage(): Promise<void> {
  // A system is disposable (explicit resource management).
  using system = createSystem();
  const counter = system.define(
    statechart<{ count: number }>({ family: 'counter', revision: '1' })
      .data('count', 0)
      .state('counting', (state) => state.on('add', (t) => t.assign('count', function () { return this.count + 1; }))),
  );
  using actor = system.spawn(counter);

  // A caller cancels a send with Node's AbortController; the API takes its signal.
  const controller = new AbortController();
  actor.post('add', undefined, { signal: controller.signal });
  await actor.send('add', undefined, { signal: controller.signal });
  controller.abort();
}
