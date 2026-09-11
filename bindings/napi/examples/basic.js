"use strict";
const { defineMachine, createSystem } = require("..");
const counter = defineMachine({
  id: "counter",
  initial: "ready",
  state: ({ input }) => ({ count: input?.count || 0 }),
  states: {
    ready: {
      on: {
        INC: {
          actions: ({ state }) => {
            state.count++;
          },
        },
      },
    },
  },
});
const system = createSystem();
const actor = system.spawn(counter, { name: "counter", input: { count: 1 } });
actor.send({ type: "INC" });
system.run();
console.log(actor.snapshot());
system.close();
