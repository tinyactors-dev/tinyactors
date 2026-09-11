"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const n = require("../lowlevel");
function ok(v) {
  assert.equal(typeof v === "number" ? v : v.status, n.TA_OK);
  return v;
}
test("normalized descriptor preserves nested state tables", () => {
  const system = ok(n.ta_system_create(null, { data: { run() {} } })).system;
  try {
    const chart = {
      name: "nested",
      data_model: "javascript",
      states: [
        {
          id: "idle",
          kind: n.TA_CHART_STATE,
          states: [
            { id: "ready", kind: n.TA_CHART_STATE },
            { id: "busy", kind: n.TA_CHART_STATE },
          ],
          transitions: [
            {
              event: "GO",
              target: "busy",
              content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] },
            },
          ],
        },
      ],
    };
    const def = ok(n.ta_definition_build(system, chart)).definition;
    const desc = n.ta_definition_description(def);
    assert.ok(Array.isArray(desc.states));
    assert.ok(Array.isArray(desc.transitions));
    assert.ok(desc.transitions[0].content.actions.length >= 1);
    const copy = ok(
      n.ta_definition_prepare(system, {
        description: desc,
        options: { storage: n.TA_DEFINITION_COPY },
      }),
    ).definition;
    assert.equal(
      n.ta_definition_description(copy).states.length,
      desc.states.length,
    );
  } finally {
    n.disposeSystem(system);
  }
});
test("constants and trace validation", () => {
  for (const key of [
    "TA_ACTOR_READY",
    "TA_INSPECT_ACTOR",
    "TA_INSPECT_DATA",
    "TA_VISIT_STOP",
    "TA_SPAN_SEND",
    "TA_PHASE_GUARD",
    "TA_DEFINITION_COPY",
    "TA_CHART_STATE",
  ])
    assert.equal(typeof n[key], "number", key);
  assert.equal(n.TA_UNLIMITED, Infinity);
});

test("JavaScript arrays cannot inject sentinels or overflow the appended element", (t) => {
  assert.throws(
    () => n.ta_system_create(null, { ioprocessors: [{}, { type: "wire" }] }),
    /type/,
  );
  assert.throws(() => n.ta_system_create(null, { services: [{}] }), /type/);
  assert.throws(
    () =>
      n.ta_system_create(null, {
        ioprocessors: [{ type: "wire", aliases: [null, "alias"] }],
      }),
    /aliases/,
  );
  const system = ok(n.ta_system_create(null, {})).system;
  t.after(() => n.disposeSystem(system));
  const chart = (states) => ({
    name: "malformed",
    data_model: "javascript",
    states,
  });
  assert.throws(
    () => n.ta_definition_build(system, chart([{}, { id: "valid" }])),
    /sentinel|id/,
  );
  assert.throws(
    () =>
      n.ta_definition_build(
        system,
        chart([{ id: "idle", transitions: [{}, { event: "GO" }] }]),
      ),
    /sentinel/,
  );
  assert.throws(
    () => n.ta_definition_build(system, chart(new Array(0xffffffff))),
    /limit|allocation|nodes/i,
  );
  assert.equal(n.bindingStats(system).native_roots, 0);
});

test("returned inline definition handles preserve round-trip execution and lifetime", (t) => {
  const system = ok(n.ta_system_create(null, {})).system;
  t.after(() => n.disposeSystem(system));
  const child = ok(
    n.ta_definition_build(system, {
      name: "child",
      data_model: "javascript",
      states: [{ id: "finished", kind: n.TA_CHART_FINAL }],
    }),
  ).definition;
  const parent = ok(
    n.ta_definition_build(system, {
      name: "parent",
      data_model: "javascript",
      states: [
        {
          id: "waiting",
          invokes: [{ inline_definition: child, id: "child" }],
          transitions: [{ event: "done.invoke.child", target: "finished" }],
        },
        { id: "finished", kind: n.TA_CHART_FINAL },
      ],
    }),
  ).definition;
  const description = n.ta_definition_description(parent);
  const inline = description.states[1].invokes[0].inline_definition;
  assert.ok(inline);
  n.ta_definition_release(parent);
  n.ta_definition_release(child);
  assert.equal(n.ta_definition_description(inline).name, "child");
  const copy = ok(n.ta_definition_prepare(system, { description })).definition;
  n.ta_definition_release(inline);
  const actor = ok(n.ta_actor_spawn(system, { definition: copy })).actor;
  ok(
    n.ta_system_run(system, {
      now_ns: 0n,
      budget: { work_units: 1000, actor_quantum: 100 },
    }),
  );
  assert.equal(
    ok(n.ta_actor_inspect(system, actor)).info.status,
    n.TA_ACTOR_DONE,
  );
  assert.deepEqual(n.takeErrors(system), []);
});
