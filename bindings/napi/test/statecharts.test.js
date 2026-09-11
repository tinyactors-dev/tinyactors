"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const {
  defineMachine,
  createSystem,
  assign,
  send,
  cancel,
  raise,
  lowlevel: n,
} = require("..");
function setup(t, options) {
  const s = createSystem(options);
  t.after(() => s.close({ force: true }));
  return s;
}

test("nested IDs, local targets, guards and history preserve configuration", (t) => {
  const machine = defineMachine({
    id: "history",
    state: () => ({ visits: 0 }),
    initial: "open",
    states: {
      open: {
        initial: "idle",
        on: { LEAVE: "closed" },
        states: {
          history: { type: "history", history: "deep", initial: "idle" },
          idle: {
            on: {
              ADVANCE: {
                target: "busy",
                guard: ({ event }) => event.data.allowed,
              },
            },
          },
          busy: {
            entry: assign(({ state }) => ({ visits: state.visits + 1 })),
          },
        },
      },
      closed: { on: { RETURN: "#open.history" } },
    },
  });
  const s = setup(t),
    a = s.spawn(machine);
  s.run();
  a.send({ type: "ADVANCE", data: { allowed: false } });
  s.run();
  assert.ok(a.snapshot().active.includes("open.idle"));
  a.send({ type: "ADVANCE", data: { allowed: true } });
  s.run();
  assert.ok(a.snapshot().active.includes("open.busy"));
  a.send("LEAVE");
  s.run();
  a.send("RETURN");
  s.run();
  assert.ok(a.snapshot().active.includes("open.busy"));
  assert.equal(a.snapshot().state.visits, 2);
  assert.deepEqual(s.takeErrors(), []);
});

test("parallel regions complete independently and onDone runs at the parent", (t) => {
  const machine = defineMachine({
    id: "parallel",
    initial: "work",
    states: {
      work: {
        type: "parallel",
        onDone: "finished",
        states: {
          first: {
            initial: "idle",
            states: { idle: { on: { A: "done" } }, done: { type: "final" } },
          },
          second: {
            initial: "idle",
            states: { idle: { on: { B: "done" } }, done: { type: "final" } },
          },
        },
      },
      finished: { type: "final" },
    },
  });
  const s = setup(t),
    a = s.spawn(machine);
  s.run();
  a.send("A");
  s.run();
  assert.ok(a.snapshot().active.includes("work.first.done"));
  assert.ok(a.snapshot().active.includes("work.second.idle"));
  a.send("B");
  s.run();
  assert.equal(a.snapshot().status, "done");
  assert.deepEqual(s.takeErrors(), []);
  a.release();
});

test("declarations, raise, delayed sends and cancel use native semantics", (t) => {
  const machine = defineMachine({
    id: "actions",
    state: () => ({}),
    data: { total: 0 },
    states: {
      idle: {
        entry: [
          assign({ ready: true }),
          raise("INIT"),
          send("LATE", { delayNs: 100n, id: "later" }),
        ],
        on: {
          INIT: ({ state }) => state.total++,
          CANCEL: cancel("later"),
          LATE: ({ state }) => (state.total += 10),
        },
      },
    },
  });
  const s = setup(t),
    a = s.spawn(machine);
  s.run();
  assert.deepEqual(a.snapshot().state, { total: 1, ready: true });
  a.send("CANCEL");
  s.run();
  s.run({ nowNs: 100n });
  assert.equal(a.snapshot().state.total, 1);
  assert.deepEqual(s.takeErrors(), []);
});

test("external invocation start, emit, done and finalize are usable from JS", (t) => {
  const s = setup(t, {
    ioprocessors: [{ type: "urn:worker-events" }],
    services: [{ type: "urn:worker", event_processor: "urn:worker-events" }],
  });
  const machine = defineMachine({
    id: "invoke",
    state: () => ({ progress: 0 }),
    states: {
      running: {
        invoke: {
          type: "urn:worker",
          id: "job",
          input: () => ({ task: 7 }),
          finalize: ({ state, event }) => {
            state.answer = event.data.answer;
          },
        },
        on: {
          PROGRESS: ({ state, event }) => {
            state.progress = event.data.n;
          },
          "done.invoke.job": "finished",
        },
      },
      finished: { type: "final" },
    },
  });
  const actor = s.spawn(machine);
  s.run();
  const lease = s.ioNext();
  assert.equal(lease.request.kind, n.TA_IO_INVOKE_START);
  assert.deepEqual(lease.request.body.content.native, { task: 7 });
  lease.complete();
  lease.emit({ type: "PROGRESS", data: { n: 2 } });
  s.run();
  assert.equal(actor.snapshot().state.progress, 2);
  lease.done({ answer: 42 });
  s.run();
  assert.equal(actor.snapshot().status, "done");
  assert.equal(actor.snapshot().state.answer, 42);
  assert.deepEqual(s.takeErrors(), []);
  actor.release();
});

test("SCXML inline child returns native done data", (t) => {
  const s = setup(t);
  const child = s.prepare(
    defineMachine({
      id: "child",
      states: { done: { type: "final", doneData: () => ({ answer: 42 }) } },
    }),
  );
  const parent = defineMachine({
    id: "parent",
    state: () => ({ answer: 0 }),
    states: {
      waiting: {
        invoke: { id: "child", inlineDefinition: child },
        on: {
          "done.invoke.child": {
            target: "done",
            actions: ({ state, event }) => {
              state.answer = event.data.answer;
            },
          },
        },
      },
      done: { type: "final" },
    },
  });
  const actor = s.spawn(parent);
  s.run();
  assert.equal(actor.snapshot().status, "done");
  assert.equal(actor.snapshot().state.answer, 42);
  assert.deepEqual(s.takeErrors(), []);
  actor.release();
});

test("cancel races preserve a taken START lease and expose reserved CANCEL", (t) => {
  const s = setup(t, {
    ioprocessors: [{ type: "urn:transport" }],
    services: [{ type: "urn:service", event_processor: "urn:transport" }],
  });
  const machine = defineMachine({
    id: "cancel-service",
    states: { running: { invoke: { id: "job", type: "urn:service" } } },
  });
  const actor = s.spawn(machine);
  s.run();
  const start = s.ioNext();
  assert.equal(start.request.kind, n.TA_IO_INVOKE_START);
  actor.stop();
  s.run();
  assert.equal(start.isCancelled(), true);
  start.complete();
  const stop = s.ioNext();
  assert.equal(stop.request.kind, n.TA_IO_INVOKE_CANCEL);
  stop.complete();
  assert.throws(
    () => start.emit("LATE"),
    (error) => error.status === n.TA_STALE || error.status === n.TA_CANCELLED,
  );
  actor.release();
  assert.equal(s.stats().invocations, 0);
});
