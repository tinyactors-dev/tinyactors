# The measurement lab

One host on `@tinyactors/wasm`, filled with as many actors as the reader
picks, then running one scenario (`lab.mjs`):

- **fill**: the host holds its actors (`counter.scxml`), each waiting;
- **events**: batches of `add` to counters drawn at random;
- **rally**: pairs of players (`player.scxml`) send a ball back and forth,
  a hundred hits a rally, with no JavaScript between hits;
- **sleep**: at most 1,000 actors in memory and the rest in a
  `MemoryStore`; each event wakes a counter from the store;
- **timers**: every actor (`timer.scxml`) holds a delayed send to itself,
  due every 1 to 10 seconds, and the lab moves a simulated clock on, 100 ms
  at a time, as fast as the host keeps up.

The website runs it in a Worker (`worker.mjs`) at
https://www.tinyactors.dev/performance/#lab and charts what it reports once
a second (`page.mjs`): actors, spawns a second, the WebAssembly memory and
memory per actor, messages a second and time per message. `main.mjs` runs
every scenario on 2,000 actors on both datamodels, seeded, for the
website's tests.

```sh
npm ci
node main.mjs
```

The host is sized for its population up front and runs with tracing off.
On the native datamodel an actor's variables are in the JavaScript heap,
which the WebAssembly memory leaves out.
