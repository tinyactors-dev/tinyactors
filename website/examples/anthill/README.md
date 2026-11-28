# Two anthills

Every ant and both queens are tinyactors actors (`ant.scxml`,
`queen.scxml`). Each colony is a host of its own, and the world,
`world.mjs`, is its ants' I/O processor: it picks where each ant goes,
tells it when it arrives, and starts the fights. After every frame a
colony hands the other a message: where its ants arrived, which enemies
they struck, the food they ate. The website runs each colony in a Worker
of its own (`worker.mjs`), the two trading frames over a
`MessageChannel`, and draws both (`page.mjs`) at
https://www.tinyactors.dev/anthill/ (what the two views share, starting
the Workers and keeping their numbers, is `field.mjs`); `main.mjs` runs two small colonies
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
`follow.mjs` shows its chart beside it (the site's `components/live-chart.mjs`, over scxmljs's `<scxml-view>`), lit in
the states the ant's host reports (`actor.state`, read after every frame
by the colony's Worker) and the transitions the ant's events fired.

## The home page's field

`hero.mjs` is the same field on the home page, beside the headline, with
no controls: 20,000 ants resting 10 simulated seconds between outings, so
most of them are out. It starts only when it comes into view, pauses both
Workers while it is out of view or its tab is hidden (`pause` in
`worker.mjs`), and with `prefers-reduced-motion` it runs eight simulated
seconds unseen and then shows that frame, still. One line under it gives
the actors in the tab and the WebAssembly memory they take, read as the
full demo reads them.

Why 20,000: measured with `launch()` in headless Chromium on a 4-CPU
Intel Xeon at 2.10 GHz (2026-11-28; the Worker's numbers, as the full
demo's charts show them), at a rest of 10 seconds:

| Ants | Hatching | Messages a second | Time per message | WebAssembly memory |
|---|---|---|---|---|
| 10,000 | 0.6 s | 3,800 | 8.5 µs | 16 MB |
| 20,000 | 0.3 s | 6,900 | 6.4 µs | 21 MB |
| 50,000 | 0.5 s | 18,000 | 4.8 µs | 39 MB |
| 100,000 | 0.7 s | 40,000 | 3.9 µs | 75 MB |

At 20,000 the two colonies work about 45 ms a second between them, under
3% of each of two cores here; a phone several times slower still keeps up
with real time with most of its cores idle (unverified on a phone), and
21 MB is a small share of a phone's memory for a picture. 50,000 would
double the memory and more than double the messages, for a field drawn
at half the full demo's width.
