"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { defineMachine } = require("../lib/compiler");
const { send, raise, cancel, assign } = require("../lib/helpers");

test("compiler emits native chart topology and code owner tokens", () => {
  const m = defineMachine({
    id: "chart",
    initial: "idle",
    state: () => ({ n: 0 }),
    states: {
      idle: {
        entry: () => {},
        on: { GO: { guard: () => true, target: "done", actions: () => {} } },
      },
      done: { final: true, doneData: () => ({ ok: true }) },
    },
  });
  assert.equal(m.chart.name, "chart");
  assert.equal(m.chart.states.length, 2);
  assert.equal(m.chart.states[0].transitions[0].event, "GO");
  assert.ok(m.owner.functions.length >= 4);
});

test("helpers compile as action descriptors", () => {
  const m = defineMachine({
    id: "helpers",
    initial: "a",
    states: {
      a: {
        on: {
          X: {
            actions: [
              send("Y"),
              raise("Z"),
              cancel("t"),
              assign(() => ({ n: 1 })),
            ],
          },
        },
      },
    },
  });
  assert.equal(m.chart.states[0].transitions[0].content.actions.length, 4);
});

test("cyclic authoring is rejected with a bounded construction error", () => {
  const state = { states: {} };
  state.states.again = state;
  assert.throws(
    () => defineMachine({ id: "cycle", states: { state } }),
    /cyclic|limit/,
  );
});
