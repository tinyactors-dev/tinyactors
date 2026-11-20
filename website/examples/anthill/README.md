# Two anthills

Every ant and both queens are tinyactors actors (`ant.scxml`,
`queen.scxml`). The world, `world.mjs`, is their I/O processor: it picks
where each ant goes, tells it when it arrives, and starts the fights. The website
runs it in a Worker (`worker.mjs`) and draws it (`page.mjs`) at
https://www.tinyactors.dev/anthill/; `main.mjs` runs a small colony in
Node.js, seeded, for the website's tests.

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
