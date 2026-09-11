"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const {
  defineMachine,
  createSystem,
  send,
  cancel,
  lowlevel: n,
} = require("..");
function system(t, options) {
  const s = createSystem(options);
  t.after(() => s.close({ force: true }));
  return s;
}

test("custom snapshot policy covers native state, messages, and transport views", async (t) => {
  class Value {
    constructor(n) {
      this.n = n;
    }
    increment() {
      this.n++;
    }
  }
  const snapshot = (value) =>
    value instanceof Value ? new Value(value.n) : structuredClone(value);
  const s = system(t, { snapshot, ioprocessors: [{ type: "wire" }] });
  const m = defineMachine({
    id: "native-class",
    state: () => new Value(1),
    states: {
      idle: {
        on: {
          INC: ({ state }) => state.increment(),
          SEND: ({ state, send: emit }) =>
            emit("VALUE", {
              processor: "wire",
              to: "wire:remote",
              data: state,
            }),
        },
      },
    },
  });
  const a = s.spawn(m);
  s.run();
  a.send("INC");
  a.send("SEND");
  s.run();
  const first = a.snapshot().state;
  assert.ok(first instanceof Value);
  first.increment();
  assert.equal(a.snapshot().state.n, 2);
  await s.drainIO((request) => {
    assert.ok(request.body.content.native instanceof Value);
    request.body.content.native.increment();
  });
  assert.equal(a.snapshot().state.n, 2);
  assert.deepEqual(s.takeErrors(), []);
});

test("JS state factories, event data, snapshots and streaming views are ergonomic", (t) => {
  const machine = defineMachine({
    id: "entity",
    state: ({ input }) => ({ x: input.x, seen: [] }),
    initial: "idle",
    states: {
      idle: {
        on: {
          MOVE: ({ state, event }) => {
            state.x += event.data.dx;
            state.seen.push(event.type);
          },
        },
      },
    },
  });
  const s = system(t),
    a = s.spawn(machine, { input: { x: 10 }, name: "player" }),
    b = s.spawn(machine, { input: { x: 20 } });
  a.send({ type: "MOVE", data: { dx: 2 } });
  s.run();
  assert.deepEqual(a.snapshot().state, { x: 12, seen: ["MOVE"] });
  assert.deepEqual(a.snapshot().active, ["idle"]);
  const saved = a.snapshot();
  saved.state.x = 1000;
  assert.equal(a.snapshot().state.x, 12);
  assert.equal(b.snapshot().state.x, 20);
  const snapshots = [];
  let result;
  do {
    result = s.inspect(
      (record) => {
        snapshots.push(record.info);
      },
      { cursor: result?.cursor, workUnits: 1 },
    );
  } while (!result.done);
  assert.equal(snapshots.length, 2);
  snapshots[0].state.x = -1;
  assert.equal(a.snapshot().state.x, 12);
  assert.deepEqual(s.takeErrors(), []);
});

test("saved execution helpers expire and context effects propagate failures", (t) => {
  let saved;
  const m = defineMachine({
    id: "context",
    state: () => ({ handled: false }),
    states: {
      idle: {
        on: {
          GO: (context) => {
            saved = context;
            context.raise("RAISED", { ok: true });
          },
          RAISED: ({ state, event }) => {
            state.handled = event.data.ok;
          },
        },
      },
    },
  });
  const s = system(t),
    a = s.spawn(m);
  a.send("GO");
  s.run();
  assert.equal(a.snapshot().state.handled, true);
  assert.throws(() => saved.raise("LATE"), /expired/);
  assert.throws(() => saved.state, /expired/);
});

test("old actor references and name-addressed sends have separate semantics", (t) => {
  const m = defineMachine({
    id: "named",
    state: () => ({ n: 0 }),
    states: { idle: { on: { INC: ({ state }) => state.n++ } } },
  });
  const s = system(t),
    a = s.spawn(m, { name: "player" }),
    b = s.spawn(m);
  s.run();
  s.bind("player", b, a);
  a.send("INC");
  s.send("player", "INC");
  s.run();
  assert.equal(a.snapshot().state.n, 1);
  assert.equal(b.snapshot().state.n, 1);
  a.stop();
  s.run();
  a.release();
  assert.equal(s.actor("player").id, b.id);
  assert.throws(
    () => a.send("INC"),
    (error) => error.status === n.TA_STALE,
  );
});

test("definitions publish before spawn and upgrade with isolated migration", (t) => {
  const first = defineMachine({
    id: "versioned",
    state: () => ({ n: 1 }),
    states: { idle: { on: { INC: ({ state }) => state.n++ } } },
  });
  const next = defineMachine({
    id: "versioned",
    state: () => ({ n: 0 }),
    states: { idle: { on: { INC: ({ state }) => (state.n += 10) } } },
  });
  const s = system(t);
  s.publish(first);
  const actor = s.spawnPublished("versioned");
  s.run();
  const oldRevision = actor.snapshot().revision;
  s.publish(next);
  assert.throws(
    () => s.upgrade(actor, next, { migrate: () => undefined }),
    (error) => error.status === n.TA_EXECUTION_ERROR,
  );
  assert.equal(actor.snapshot().revision, oldRevision);
  assert.deepEqual(actor.snapshot().state, { n: 1 });
  assert.equal(s.takeErrors().length, 1);
  s.upgrade(actor, next, {
    expectedRevision: oldRevision,
    migrate: ({ state }) => ({ ...state, upgraded: true }),
  });
  actor.send("INC");
  s.run();
  assert.deepEqual(actor.snapshot().state, { n: 11, upgraded: true });
  assert.notEqual(actor.snapshot().revision, oldRevision);
  s.releaseDefinition(first);
});

test("a failed outgoing snapshot completes the already-taken lease", (t) => {
  let fail = false;
  const s = system(t, {
    ioprocessors: [{ type: "wire" }],
    snapshot(value) {
      if (fail) throw new Error("snapshot failed");
      return structuredClone(value);
    },
  });
  const actor = s.spawn(
    defineMachine({
      id: "snapshot-failure",
      state: () => ({ failed: false }),
      states: {
        idle: {
          on: {
            GO: send("OUT", {
              processor: "wire",
              to: "wire:remote",
              data: { value: 1 },
            }),
            "error.communication": ({ state }) => {
              state.failed = true;
            },
          },
        },
      },
    }),
  );
  actor.send("GO");
  s.run();
  assert.equal(s.stats().io_requests, 1);
  fail = true;
  assert.throws(() => s.ioNext(), /snapshot failed/);
  fail = false;
  s.run();
  assert.equal(s.stats().io_requests, 0);
  assert.equal(actor.snapshot().state.failed, true);
  assert.deepEqual(s.takeErrors(), []);
});

test("limits surface backpressure as typed errors", (t) => {
  const m = defineMachine({ id: "limited", states: { idle: {} } }),
    s = system(t, {
      limits: { actors: 1 },
      actorLimits: { externalEvents: 1 },
    });
  const a = s.spawn(m);
  assert.throws(
    () => s.spawn(m),
    (error) => error.status === n.TA_LIMIT,
  );
  a.send("A");
  assert.throws(
    () => a.send("B"),
    (error) => error.status === n.TA_LIMIT,
  );
});

test("I/O leases are asynchronous host work with exactly-once completion", async (t) => {
  const m = defineMachine({
    id: "sender",
    states: {
      idle: {
        on: {
          GO: send("update", {
            processor: "websocket",
            to: "ws:browser",
            data: () => ({ x: 42 }),
          }),
        },
      },
    },
  });
  const s = system(t, {
    ioprocessors: [{ type: "urn:websocket", aliases: ["websocket"] }],
  });
  const a = s.spawn(m);
  a.send("GO");
  s.run();
  assert.deepEqual(s.takeErrors(), []);
  let saved;
  assert.equal(
    await s.drainIO(async (request, lease) => {
      saved = lease;
      assert.equal(request.type, "urn:websocket");
      assert.equal(request.target, "ws:browser");
      assert.deepEqual(request.body.content.native, { x: 42 });
      await Promise.resolve();
    }),
    1,
  );
  assert.throws(
    () => saved.complete(),
    (error) => error.status === n.TA_STALE,
  );
  assert.equal(s.ioNext(), null);
});

test("graceful close runs exit actions and refuses to silently abandon I/O", (t) => {
  let exits = 0;
  const m = defineMachine({
    id: "close",
    states: {
      idle: {
        exit: () => {
          exits++;
        },
        on: { GO: send("X", { processor: "external", to: "urn:peer" }) },
      },
    },
  });
  const s = system(t, { ioprocessors: [{ type: "external" }] }),
    a = s.spawn(m);
  a.send("GO");
  s.run();
  assert.throws(
    () => s.close(),
    (error) => error.status === n.TA_BUSY,
  );
  assert.equal(exits, 1);
  const lease = s.ioNext();
  assert.ok(lease);
  lease.complete(n.TA_CANCELLED);
  s.close();
  s.close();
});

test("automatic scheduler advances manual-time timers and wakes for messages", async (t) => {
  const m = defineMachine({
    id: "timer",
    state: () => ({ fired: false }),
    states: {
      waiting: {
        entry: send("FIRE", { delayMs: 5, id: "timer" }),
        on: {
          FIRE: {
            target: "done",
            actions: ({ state }) => {
              state.fired = true;
            },
          },
        },
      },
      done: {},
    },
  });
  const s = system(t),
    actor = s.spawn(m);
  s.run({ nowNs: 10n });
  s.start({ workUnits: 100 });
  await new Promise((resolve) => setTimeout(resolve, 30));
  s.stop();
  assert.equal(actor.snapshot().state.fired, true);
  assert.deepEqual(s.takeErrors(), []);
});

test("ESM exports the same actor API", async () => {
  const module = await import("../index.mjs");
  assert.equal(typeof module.defineMachine, "function");
  assert.equal(typeof module.createSystem, "function");
});
