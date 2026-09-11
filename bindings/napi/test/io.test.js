"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const n = require("../lowlevel");
function ok(x) {
  assert.equal(
    typeof x === "number" ? x : x.status,
    n.TA_OK,
    require("node:util").inspect(x),
  );
  return x;
}
function run(s) {
  return ok(
    n.ta_system_run(s, {
      now_ns: 1n,
      budget: { work_units: 10000, actor_quantum: 100 },
    }),
  );
}
function build(s, extra = {}) {
  return ok(
    n.ta_definition_build(s, {
      name: "io",
      data_model: "javascript",
      states: [{ id: "idle" }],
      ...extra,
    }),
  ).definition;
}

test("processors aliases resolve and SEND leases complete exactly once", (t) => {
  const locations = [];
  const s = ok(
    n.ta_system_create(null, {
      ioprocessors: [
        {
          type: "urn:test",
          aliases: ["test"],
          location(context) {
            locations.push(context);
            return "local:test";
          },
        },
      ],
      data: {
        run(context) {
          return n.ta_context_send(context, {
            event: "PING",
            type: "urn:test",
            target: { kind: n.TA_TARGET_URI, text: "remote:test" },
            body: {
              kind: n.TA_BODY_CONTENT,
              content: { kind: n.TA_PAYLOAD_NATIVE, native: { ok: true } },
            },
          }).status;
        },
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(s));
  const d = build(s, {
    states: [
      {
        id: "idle",
        transitions: [
          {
            event: "GO",
            content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] },
          },
        ],
      },
    ],
  });
  const a = ok(n.ta_actor_spawn(s, { definition: d })).actor;
  run(s);
  ok(
    n.ta_post(s, {
      target: { kind: n.TA_TARGET_INSTANCE, instance: a },
      message: { name: "GO", data: { kind: n.TA_PAYLOAD_NONE } },
    }),
  );
  run(s);
  const q = ok(n.ta_io_next(s)).request;
  assert.equal(q.type, "urn:test");
  assert.equal(q.target, "remote:test");
  assert.equal(q.body.content.native.ok, true);
  assert.equal(locations.length, 1);
  ok(n.ta_io_complete(s, q.request, n.TA_OK));
  assert.equal(n.ta_io_complete(s, q.request, n.TA_OK), n.TA_STALE);
  assert.equal(n.ta_io_next(s).status, n.TA_EMPTY);
  assert.deepEqual(n.takeErrors(s), []);
});

test("I/O receive preserves metadata and receive errors enqueue communication failure", (t) => {
  const seen = [];
  const s = ok(
    n.ta_system_create(null, {
      ioprocessors: [{ type: "urn:test" }],
      data: {
        bind_event(context, event) {
          seen.push(event);
        },
      },
    }),
  ).system;
  t.after(() => n.disposeSystem(s));
  const d = build(s, {
    states: [
      {
        id: "idle",
        transitions: [{ event: "error.communication", target: "idle" }],
      },
    ],
  });
  const a = ok(n.ta_actor_spawn(s, { definition: d })).actor;
  run(s);
  const route = { processor: "urn:test", destination: a };
  ok(
    n.ta_io_receive(s, route, {
      name: "HELLO",
      type: n.TA_EVENT_EXTERNAL,
      origin: "remote",
      origintype: "scxml",
      data: { kind: n.TA_PAYLOAD_NATIVE, native: { n: 4 } },
    }),
  );
  run(s);
  ok(n.ta_io_receive_error(s, route, "send-1"));
  run(s);
  assert.equal(seen[0].name, "HELLO");
  assert.equal(seen[0].origin, "remote");
  assert.ok(seen.some((x) => x.name === "error.communication"));
  assert.deepEqual(n.takeErrors(s), []);
});
