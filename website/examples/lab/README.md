# The measurement lab

One host on `@tinyactors/wasm`, its population following a pattern over
time up to a peak the reader picks (`lab.mjs`): steady, a ramp, waves or
bursts. The lab spawns actors when there are fewer than the pattern asks
for and ends actors drawn at random when there are more, and the
scenario's work goes to the actors alive:

- **idle**: the actors (`counter.scxml`) wait for an event;
- **events**: batches of `add` to counters drawn at random;
- **rally**: pairs of players (`player.scxml`) send a ball back and forth,
  a hundred hits a rally, with no JavaScript between hits;
- **sleep**: at most 1,000 actors in memory and the rest in a
  `MemoryStore`; each event wakes a counter from the store;
- **timers**: every actor (`timer.scxml`) holds a delayed send to itself,
  due every 1 to 10 seconds, and the lab moves a simulated clock on, 100 ms
  at a time, as fast as the host keeps up.

The website runs it in a Worker (`worker.mjs`) at
https://www.tinyactors.dev/performance/#lab and charts the whole run, a
point a second (`page.mjs`, on `components/timeline.mjs`): the actors
alive against the pattern, spawned and ended a second, messages a second,
time per message, and the WebAssembly memory (and the store). `main.mjs`
runs every scenario on 2,000 actors, steady, on both datamodels, checked
against what the actors counted, then each pattern for two minutes,
seeded, for the website's tests.

```sh
npm ci
node main.mjs
```

The host is sized for its peak up front (a pool that runs out grows by
copying the region, and WebAssembly memory keeps its peak) and runs with
tracing off. On the native datamodel an actor's variables are in the
JavaScript heap, which the WebAssembly memory leaves out.
