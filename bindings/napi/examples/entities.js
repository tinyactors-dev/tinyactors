"use strict";
const assert = require("node:assert/strict");
const { defineMachine, createSystem } = require("..");
const count = Number(process.argv[2] || 200000);
if (!Number.isSafeInteger(count) || count < 1)
  throw new RangeError("Entity count must be a positive integer.");
const entity = defineMachine({
  id: "entity",
  state: ({ input }) => ({ x: input, y: 0, vx: 1, vy: 2, sprite: 7 }),
  states: {
    alive: {
      on: {
        TICK: ({ state, event }) => {
          state.x += state.vx * event.data.dt;
          state.y += state.vy * event.data.dt;
        },
      },
    },
  },
});
const simulation = createSystem({ limits: { actors: count } });
const actors = new Array(count);
const start = performance.now();
for (let i = 0; i < count; i++)
  actors[i] = simulation.spawn(entity, { input: i });
while (simulation.run().runnable) {}
if (global.gc) global.gc();
const idle = {
  milliseconds: performance.now() - start,
  core: simulation.stats().memory.reserved_bytes,
  process: process.memoryUsage(),
};
const tick = performance.now();
for (const actor of actors) actor.send({ type: "TICK", data: { dt: 1 } });
while (simulation.run().runnable) {}
const tickMilliseconds = performance.now() - tick;
let sum = 0,
  visited = 0,
  page;
const inspection = performance.now();
do {
  page = simulation.inspect(
    (record) => {
      sum += record.info.state.x;
      visited++;
    },
    { cursor: page?.cursor, workUnits: 1024 },
  );
} while (!page.done);
assert.equal(visited, count);
assert.equal(sum, (count * (count - 1)) / 2 + count);
assert.deepEqual(simulation.takeErrors(), []);
console.log(
  JSON.stringify(
    {
      actors: count,
      idle,
      tickMilliseconds,
      inspectionMilliseconds: performance.now() - inspection,
    },
    null,
    2,
  ),
);
simulation.close();
