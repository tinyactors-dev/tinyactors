# Two anthills

Every ant and both queens are tinyactors actors (`ant.scxml`,
`queen.scxml`). The world, `world.mjs`, is their I/O processor: it picks
each ant's legs, says when they end, and starts the fights. The website
runs it in a Worker (`worker.mjs`) and draws it (`page.mjs`) at
https://www.tinyactors.dev/anthill/; `main.mjs` runs a small colony in
Node.js, seeded, for the website's tests.

```sh
npm ci
node main.mjs
```

An ant decides only when something happens to it: it leaves, finds food,
comes home, rests, meets an enemy. The world records each leg (two points,
two times), and the page draws every ant between its decisions, so drawing
costs tinyactors nothing. What limits the population is decisions a
second.

## Measured

| ants | ready | drawn | sim speed | decisions a second |
|---|---|---|---|---|
| 100,000 | 1.1 s | 60 fps | 1.00× | 20,580 |
| 250,000 | 2.5 s | 61 fps | 1.00× | 55,365 |
| 500,000 | 4.9 s | 60 fps | 0.96× | 111,952 |
| 1,000,000 | 9.5 s | 51 fps | 0.58× | 116,264 |

## How it was measured

Measured 2026-11-20 in headless Chromium 157 on an Intel Xeon at 2.10 GHz
(4 CPUs, 16 GiB, Linux), with @tinyactors/wasm 0.4.0, the native
datamodel and tracing off. Each population started from the page's slider;
"ready" is from the click to the first frame, "drawn" frames a second
right after, and the sim speed and decisions a second the mean of 30
readings of the page's own counters, a second apart, after the first 30
seconds. A million ants decide more often than the host can answer on
this machine, so the simulation runs at about half real time while the
page still draws every ant.
