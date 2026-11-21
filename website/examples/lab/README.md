# The measurement lab

One host on `@tinyactors/wasm` running one scenario (`lab.mjs`):

- **cells**: a colony whose population is the cells' own doing. Each cell
  (`cell.scxml`) burns a unit of energy a simulated second and ages; the
  light, the lab's I/O processor `urn:lab`, feeds cells drawn at random,
  as much food a second as the cells the reader picked burn, and names a
  neighbour to each; a cell with energy to spare gives some to its
  neighbour (a send to `#_scxml_<session>`), one with a full store divides
  (the light spawns the daughter), and a cell dies of hunger or of age in
  a final state, which the host reports (`onDone`). Births, deaths and the
  events handled are the core's own counts (`hostStats`).

The other scenarios hold a fixed population:

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
alive, born and died (or spawned) a second, events a second, time per
event, and the WebAssembly memory (and the store). `main.mjs` runs a
colony for two simulated minutes and every other scenario on 2,000
actors, on both datamodels, seeded, checked against what the actors
counted, for the website's tests.

```sh
npm ci
node main.mjs
```

The host is sized for its population up front (a colony for twice the cells its light feeds) (a pool that runs out grows by
copying the region, and WebAssembly memory keeps its peak) and runs with
tracing off. On the native datamodel an actor's variables are in the
JavaScript heap, which the WebAssembly memory leaves out.
