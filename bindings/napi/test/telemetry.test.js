"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const n = require("../lowlevel");

function ok(x) {
  assert.equal(typeof x === "number" ? x : x.status, n.TA_OK);
  return x;
}
function chart(s, extra = {}) {
  return ok(
    n.ta_definition_build(s, {
      name: "telemetry",
      data_model: "javascript",
      states: [{ id: "idle" }],
      ...extra,
    }),
  ).definition;
}
function run(s) {
  return ok(
    n.ta_system_run(s, {
      now_ns: 1n,
      budget: { work_units: 10000, actor_quantum: 100 },
    }),
  );
}

test("trace start/end preserve handle, IDs, and links", (t) => {
  const calls = { start: 0, end: 0 };
  const ids = Buffer.alloc(16, 7),
    span = Buffer.alloc(8, 3);
  const handle = { span: true };
  const system = ok(
    n.ta_system_create(null, {
      trace: {
        start(x) {
          calls.start++;
          assert.equal(x.operation, n.TA_SPAN_PROCESS);
          assert.ok(x.actor > 0n);
          assert.ok(Array.isArray(x.links));
          return {
            handle,
            context: {
              trace_id: ids,
              span_id: span,
              trace_flags: 1,
              remote: 0,
            },
          };
        },
        end(x) {
          calls.end++;
          assert.strictEqual(x.handle, handle);
          assert.deepEqual(x.context.trace_id, ids);
          assert.deepEqual(x.context.span_id, span);
        },
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(system));
  const d = chart(system, {
      states: [{ id: "idle", transitions: [{ event: "PING" }] }],
    }),
    a = ok(n.ta_actor_spawn(system, { definition: d })).actor;
  run(system);
  ok(
    n.ta_post(system, {
      target: { kind: n.TA_TARGET_INSTANCE, instance: a },
      message: { name: "PING", data: { kind: n.TA_PAYLOAD_NONE } },
    }),
  );
  run(system);
  assert.ok(calls.start >= 1);
  assert.ok(calls.end >= 1);
  ok(n.ta_actor_stop(system, a));
  run(system);
  ok(n.ta_actor_release(system, a));
  assert.ok(calls.end >= calls.start);
});

test("telemetry failures do not alter execution or leak roots", (t) => {
  let stateRuns = 0;
  const s = ok(
    n.ta_system_create(null, {
      data: {
        run() {
          stateRuns++;
        },
      },
      trace: {
        start() {
          throw new Error("trace start failed");
        },
        end() {
          throw new Error("trace end failed");
        },
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(s));
  const d = chart(s, {
    states: [
      {
        id: "idle",
        transitions: [
          {
            event: "PING",
            content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] },
          },
        ],
      },
    ],
  });
  const a = ok(n.ta_actor_spawn(s, { definition: d })).actor;
  run(s);
  const baseline = n.bindingStats(s).native_roots;
  ok(
    n.ta_post(s, {
      target: { kind: n.TA_TARGET_INSTANCE, instance: a },
      message: { name: "PING", data: { kind: n.TA_PAYLOAD_NONE } },
    }),
  );
  run(s);
  assert.ok(stateRuns >= 1);
  assert.equal(n.bindingStats(s).native_roots, baseline);
  assert.ok(n.takeErrors(s).length >= 1);
});

test("trace pair is required and optional hooks remain optional", () => {
  assert.throws(
    () => n.ta_system_create(null, { trace: { start() {} } }),
    /trace|start|end/i,
  );
  assert.throws(
    () => n.ta_system_create(null, { trace: { start: 1, end: 2 } }),
    /function/,
  );
  assert.throws(() => n.ta_system_create(null, { snapshot: true }), /function/);
  const s = ok(n.ta_system_create(null, {})).system;
  try {
    assert.equal(typeof n.ta_system_stats(s).actors, "number");
  } finally {
    n.disposeSystem(s);
  }
});

test("send and process spans preserve parents and links and release throwing end hooks", (t) => {
  const starts = [],
    ends = [];
  let receiver;
  const system = ok(
    n.ta_system_create(null, {
      data: {
        run(context) {
          return n.ta_context_send(context, {
            event: "DELIVERY",
            target: { kind: n.TA_TARGET_INSTANCE, instance: receiver },
          }).status;
        },
      },
      trace: {
        start(input) {
          const context = {
            trace_id: Buffer.alloc(16, 1),
            span_id: Buffer.alloc(8, starts.length + 1),
            trace_flags: 1,
          };
          const handle = { input, context };
          starts.push(handle);
          return { handle, context };
        },
        end(input) {
          ends.push(input.handle);
          throw new Error("export failed");
        },
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(system));
  const definition = chart(system, {
    states: [
      {
        id: "idle",
        transitions: [
          {
            event: "GO",
            content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] },
          },
          { event: "DELIVERY" },
        ],
      },
    ],
  });
  const sender = ok(n.ta_actor_spawn(system, { definition })).actor;
  receiver = ok(n.ta_actor_spawn(system, { definition })).actor;
  run(system);
  starts.length = 0;
  ends.length = 0;
  n.takeErrors(system);
  const baseline = n.bindingStats(system).native_roots;
  const parent = {
    trace_id: Buffer.alloc(16, 1),
    span_id: Buffer.alloc(8, 99),
  };
  ok(
    n.ta_post(system, {
      target: { kind: n.TA_TARGET_INSTANCE, instance: sender },
      message: { name: "GO", trace: parent },
    }),
  );
  run(system);
  const processing = starts.find(
    (item) =>
      item.input.operation === n.TA_SPAN_PROCESS && item.input.actor === sender,
  );
  const sending = starts.find(
    (item) => item.input.operation === n.TA_SPAN_SEND,
  );
  const delivery = starts.find(
    (item) =>
      item.input.operation === n.TA_SPAN_PROCESS &&
      item.input.actor === receiver,
  );
  assert.equal(processing.input.parent, null);
  assert.deepEqual(processing.input.links[0].span_id, parent.span_id);
  assert.deepEqual(sending.input.parent.span_id, processing.context.span_id);
  assert.deepEqual(delivery.input.links[0].span_id, sending.context.span_id);
  assert.equal(starts.length, 3);
  assert.equal(ends.length, 3);
  assert.equal(new Set(ends).size, 3);
  assert.equal(n.bindingStats(system).native_roots, baseline);
  assert.equal(n.takeErrors(system).length, 3);
});

test("numeric validation status rejects definition", (t) => {
  const s = ok(
    n.ta_system_create(null, {
      validate_code() {
        return n.TA_INVALID;
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(s));
  assert.equal(
    n.ta_definition_build(s, {
      name: "bad",
      data_model: "javascript",
      states: [{ id: "idle" }],
    }).status,
    n.TA_INVALID,
  );
});

test("diagnostic and terminal hooks are callable", (t) => {
  let terminal = 0,
    diagnostic = 0;
  const s = ok(
    n.ta_system_create(null, {
      diagnostic() {
        diagnostic++;
      },
      terminal() {
        terminal++;
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(s));
  const d = chart(s),
    a = ok(n.ta_actor_spawn(s, { definition: d })).actor;
  run(s);
  ok(n.ta_actor_stop(s, a));
  run(s);
  ok(n.ta_actor_release(s, a));
  assert.equal(typeof diagnostic, "number");
  assert.equal(terminal, 1);
});
