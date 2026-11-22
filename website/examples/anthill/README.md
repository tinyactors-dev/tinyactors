# Two anthills

Every ant and both queens are tinyactors actors (`ant.scxml`,
`queen.scxml`). Each colony is a host of its own, and the world,
`world.mjs`, is its ants' I/O processor: it picks where each ant goes,
tells it when it arrives, and starts the fights. After every frame a
colony hands the other a message: where its ants arrived, which enemies
they struck, the food they ate. The website runs each colony in a Worker
of its own (`worker.mjs`), the two trading frames over a
`MessageChannel`, and draws both (`page.mjs`) at
https://www.tinyactors.dev/anthill/; `main.mjs` runs two small colonies
in one Node.js process, taking turns, seeded, for the website's tests.

```sh
npm ci
node main.mjs
```

An ant decides only when something happens to it: it leaves, finds food,
comes home, rests, meets an enemy. The world records each trip (two points,
two times), and the page draws every ant between its decisions, so drawing
costs tinyactors nothing. What limits the population is decisions a
second.

The page measures itself while it runs: frames drawn a second, the
simulation's speed against real time, the size of tinyactors's
WebAssembly memory (read from the instance as the Worker makes it), and
decisions a second. The ants are the design system's rigged ant
(`sprites.mjs`), walking on the GPU (`renderer.mjs`); without a GPU, each
ant is a dot.

A reader can follow one ant: the view keeps it in the middle, and
`follow.mjs` shows its chart beside it in scxmljs's `<scxml-view>`, lit in
the states the ant's host reports (`actor.state`, read after every frame
by the colony's Worker) and the transitions the ant's events fired.
