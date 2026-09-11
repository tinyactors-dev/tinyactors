"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { spawnSync } = require("node:child_process");
const n = require("../lowlevel");
function ok(result) {
  assert.equal(
    typeof result === "number" ? result : result.status,
    n.TA_OK,
    JSON.stringify(result, (_, v) => (typeof v === "bigint" ? String(v) : v)),
  );
  return result;
}
function runtime(t, config = {}) {
  const system = ok(n.ta_system_create(null, config)).system;
  t.after(() => {
    try {
      n.disposeSystem(system);
    } catch (e) {
      if (!/closed/.test(e.message)) throw e;
    }
  });
  return system;
}
function chart(system, extra = {}) {
  return ok(
    n.ta_definition_build(system, {
      name: "test",
      data_model: "javascript",
      states: [{ id: "idle" }],
      ...extra,
    }),
  ).definition;
}
function run(system, now = 0n) {
  return ok(
    n.ta_system_run(system, {
      now_ns: now,
      budget: { work_units: 10000, actor_quantum: 100 },
    }),
  ).result;
}
function post(system, actor, event) {
  return ok(
    n.ta_post(system, {
      target: { kind: n.TA_TARGET_INSTANCE, instance: actor },
      message: {
        name: event.name,
        data: { kind: n.TA_PAYLOAD_NATIVE, native: event.data },
        trace: event.trace,
      },
    }),
  );
}

test("every public C function and named value is exported", () => {
  const header = readFileSync(
    require("node:path").resolve(__dirname, "../../../src/tinyactors.h"),
    "utf8",
  );
  const names = [...header.matchAll(/TA_API\s+[^;]+?\b(ta_\w+)\s*\(/g)].map(
    (match) => match[1],
  );
  assert.equal(names.length, 54);
  for (const name of names) assert.equal(typeof n[name], "function", name);
  assert.equal(n.ta_text_from_cstr("abc\0def"), "abc");
  assert.equal(n.TA_TIME_NEVER, (1n << 64n) - 1n);
  assert.equal(typeof n.TA_TARGET_NAME, "number");
});

test("defaults round-trip and zero limits remain real limits", (t) => {
  const config = n.ta_system_config_init();
  const system = runtime(t, config);
  const definition = chart(system);
  ok(n.ta_system_set_limits(system, { actors: 0 }));
  assert.equal(n.ta_actor_spawn(system, { definition }).status, n.TA_LIMIT);
  ok(n.ta_system_set_limits(system, { actors: 2 }));
  ok(n.ta_actor_spawn(system, { definition }));
  run(system);
  assert.equal(n.ta_system_stats(system).actors, 1);
});

test("native state and message values are isolated, rooted, and released", (t) => {
  const system = runtime(t, {
    data: {
      run(context) {
        const view = n.ta_context_inspect(context);
        view.state.native.received.push(view.system.event.data.native.n);
      },
    },
  });
  const definition = chart(system, {
    states: [
      {
        id: "idle",
        transitions: [
          {
            event: "ADD",
            content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] },
          },
        ],
      },
    ],
  });
  const seed = { received: [] };
  const a = ok(n.ta_actor_spawn(system, { definition, seed })).actor;
  const b = ok(n.ta_actor_spawn(system, { definition, seed })).actor;
  seed.received.push("sender");
  run(system);
  const payload = { n: 7 };
  post(system, a, { name: "ADD", data: payload });
  payload.n = 9;
  if (global.gc) global.gc();
  run(system);
  assert.deepEqual(ok(n.ta_actor_inspect(system, a)).info.state.native, {
    received: [7],
  });
  assert.deepEqual(ok(n.ta_actor_inspect(system, b)).info.state.native, {
    received: [],
  });
  assert.deepEqual(n.takeErrors(system), []);
  ok(n.ta_actor_stop(system, a));
  ok(n.ta_actor_stop(system, b));
  run(system);
  ok(n.ta_actor_release(system, a));
  ok(n.ta_actor_release(system, b));
  assert.equal(n.bindingStats(system).native_roots, 0);
});

test("contexts expire, reentry is rejected, and thrown actions become error.execution", (t) => {
  let escaped;
  const system = runtime(t, {
    data: {
      run(context, kind, code) {
        escaped = context;
        if (code === 1n) {
          assert.throws(
            () => n.ta_actor_stop(system, n.ta_context_inspect(context).actor),
            /Reentrant/,
          );
          throw new Error("action failed");
        }
        n.ta_context_inspect(context).state.native.handled = true;
      },
    },
  });
  const definition = chart(system, {
    states: [
      {
        id: "idle",
        onentry: [{ actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] }],
        transitions: [
          {
            event: "error.execution",
            content: { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 2n }] },
          },
        ],
      },
    ],
  });
  const actor = ok(n.ta_actor_spawn(system, { definition })).actor;
  run(system);
  assert.equal(
    ok(n.ta_actor_inspect(system, actor)).info.state.native.handled,
    true,
  );
  assert.match(n.takeErrors(system)[0].message, /action failed/);
  assert.throws(() => n.ta_context_inspect(escaped), /expired/);
});

test("effect status aborts a block and promises are rejected", (t) => {
  let extra = 0;
  const system = runtime(t, {
    data: {
      run(context, kind, code) {
        if (code === 1n) return n.TA_EXECUTION_ERROR;
        if (code === 2n) extra++;
        if (code === 3n) return Promise.resolve();
      },
    },
  });
  const definition = chart(system, {
    states: [
      {
        id: "idle",
        onentry: [
          {
            actions: [
              { kind: n.TA_ACTION_CUSTOM, code: 1n },
              { kind: n.TA_ACTION_CUSTOM, code: 2n },
            ],
          },
          { actions: [{ kind: n.TA_ACTION_CUSTOM, code: 3n }] },
        ],
      },
    ],
  });
  ok(n.ta_actor_spawn(system, { definition }));
  run(system);
  assert.equal(extra, 0);
  assert.match(n.takeErrors(system)[0].message, /synchronous/);
});

test("streaming inspection pauses, resumes, and rejects stale cursors", (t) => {
  const system = runtime(t),
    definition = chart(system);
  const a = ok(n.ta_actor_spawn(system, { definition })).actor;
  ok(n.ta_actor_spawn(system, { definition }));
  run(system);
  const seen = [];
  const options = {
    details: n.TA_INSPECT_DATA | n.TA_INSPECT_ACTIVE,
    work_units: 100,
    visit(record) {
      seen.push(record);
      return n.TA_VISIT_PAUSE;
    },
  };
  let result = n.ta_system_inspect(system, options);
  assert.equal(result.status, n.TA_AGAIN);
  while (result.status === n.TA_AGAIN)
    result = n.ta_system_inspect(system, options, result.cursor);
  assert.equal(result.status, n.TA_OK);
  assert.equal(
    seen.filter((record) => record.kind === n.TA_INSPECT_ACTOR).length,
    2,
  );
  result = n.ta_system_inspect(system, options);
  post(system, a, { name: "X" });
  assert.equal(
    n.ta_system_inspect(system, options, result.cursor).status,
    n.TA_STALE,
  );
});

test("arena memory is copied and invalidated with its lifetime", () => {
  const options = {
    initial_chunk_bytes: 256,
    maximum_chunk_bytes: 4096,
    maximum_bytes: Infinity,
    maximum_allocation: 4096,
  };
  const arena = ok(n.ta_arena_create(null, options)).arena;
  const child = ok(n.ta_arena_open(arena, options)).arena;
  const memory = ok(n.ta_arena_alloc(child, { size: 4, alignment: 4 })).memory;
  n.memoryWrite(memory, Buffer.from([1, 2, 3, 4]));
  const bytes = n.memoryRead(memory);
  bytes[0] = 9;
  assert.equal(n.memoryRead(memory)[0], 1);
  const order = [];
  ok(n.ta_arena_defer(child, (x) => order.push(x), 1));
  ok(n.ta_arena_defer(child, (x) => order.push(x), 2));
  n.ta_arena_close(arena);
  assert.deepEqual(order, [2, 1]);
  assert.throws(() => n.memoryRead(memory), /lifetime|closed/);
  assert.deepEqual([...bytes], [9, 2, 3, 4]);
});

test("runtime arenas and byte state never expose dangling buffers", (t) => {
  let memory, borrowed, saved;
  const system = runtime(t, {
    data: {
      run(context) {
        const result = ok(n.ta_context_bytes(context));
        saved = result.bytes;
        saved.writeUInt32LE(42);
        ok(n.contextWriteBytes(context, saved));
        borrowed = ok(
          n.ta_context_arena(context, n.TA_LIFETIME_MACROSTEP),
        ).arena;
        memory = ok(
          n.ta_arena_alloc(borrowed, { size: 8, alignment: 8 }),
        ).memory;
        assert.throws(() => n.ta_arena_close(borrowed), /runtime-owned/);
      },
    },
  });
  const definition = chart(system, {
    data_layout: {
      storage: n.TA_DATA_BYTES,
      bytes_layout: { size: 4, alignment: 4 },
    },
    states: [
      {
        id: "idle",
        onentry: [{ actions: [{ kind: n.TA_ACTION_CUSTOM, code: 1n }] }],
      },
    ],
  });
  const actor = ok(n.ta_actor_spawn(system, { definition })).actor;
  run(system);
  assert.equal(
    ok(n.ta_actor_inspect(system, actor)).info.state.bytes.readUInt32LE(),
    42,
  );
  saved.writeUInt32LE(7);
  assert.equal(
    ok(n.ta_actor_inspect(system, actor)).info.state.bytes.readUInt32LE(),
    42,
  );
  assert.throws(() => n.memoryRead(memory), /lifetime|closed/);
});

test("handles and integer boundaries reject before unsafe native operations", (t) => {
  const system = runtime(t),
    other = runtime(t),
    definition = chart(system);
  assert.throws(() => n.ta_actor_inspect({}, 1n), /opaque handle/);
  assert.throws(
    () => n.ta_actor_inspect(system, Number.MAX_SAFE_INTEGER + 1),
    /BigInt/,
  );
  assert.throws(() => n.ta_actor_inspect(system, -1n), /uint64/);
  assert.throws(
    () => n.ta_actor_spawn(other, { definition }),
    /another actor system/,
  );
  assert.throws(() => n.ta_actor_spawn(definition, {}), /handle kind/);
  const cycle = { id: "cycle" };
  cycle.states = [cycle];
  assert.throws(() => chart(system, { states: [cycle] }), /depth/);
});

test("name replacement preserves incarnation identity and current routing", (t) => {
  const system = runtime(t),
    definition = chart(system);
  const old = ok(
    n.ta_actor_spawn(system, { definition, name: "player" }),
  ).actor;
  const current = ok(n.ta_actor_spawn(system, { definition })).actor;
  run(system);
  ok(n.ta_name_bind(system, { name: "player", expected: old }, current));
  assert.equal(ok(n.ta_name_lookup(system, "player")).actor, current);
  ok(n.ta_actor_stop(system, old));
  run(system);
  ok(n.ta_actor_release(system, old));
  assert.equal(ok(n.ta_name_lookup(system, "player")).actor, current);
  assert.equal(n.ta_actor_inspect(system, old).status, n.TA_STALE);
});

test("publication and native-state upgrade preserve the actor identity", (t) => {
  const system = runtime(t),
    first = chart(system),
    second = chart(system);
  ok(
    n.ta_definition_publish(
      system,
      { key: "entity", expected_revision: 0n },
      first,
    ),
  );
  const actor = ok(
    n.ta_actor_spawn(system, { definition_key: "entity", seed: { x: 1 } }),
  ).actor;
  run(system);
  ok(
    n.ta_definition_publish(
      system,
      { key: "entity", expected_revision: n.ta_definition_revision(first) },
      second,
    ),
  );
  ok(
    n.ta_actor_upgrade(system, {
      actor,
      replacement: second,
      migrate(migration) {
        return { ...migration.old_state.native, upgraded: true };
      },
    }),
  );
  assert.deepEqual(ok(n.ta_actor_inspect(system, actor)).info.state.native, {
    x: 1,
    upgraded: true,
  });
  assert.equal(
    ok(n.ta_actor_inspect(system, actor)).info.revision,
    n.ta_definition_revision(second),
  );
});

test("abandoned handles are safe during GC and environment teardown", () => {
  const modulePath = require.resolve("../lowlevel");
  const child = spawnSync(
    process.execPath,
    [
      "--expose-gc",
      "-e",
      `
    const n=require(${JSON.stringify(modulePath)});
    for(let i=0;i<50;i++) {
      const {system}=n.ta_system_create(null,{});
      const {definition}=n.ta_definition_build(system,{name:'gc',data_model:'javascript',states:[{id:'idle'}]});
      n.ta_actor_spawn(system,{definition,seed:{x:i}});
      n.ta_system_run(system,{now_ns:10n,budget:{work_units:100,actor_quantum:10}});
    }
    global.gc(); setImmediate(()=>global.gc());
  `,
    ],
    { encoding: "utf8", timeout: 10000 },
  );
  assert.equal(child.status, 0, child.stderr || child.error?.message);
});

test("BORROW leases and code owners stay pinned through the last actor", (t) => {
  const system = runtime(t),
    original = chart(system);
  const description = n.ta_definition_description(original);
  description.code_owner = { functions: [() => 42] };
  let releases = 0;
  const definition = ok(
    n.ta_definition_prepare(system, {
      description,
      options: {
        storage: n.TA_DEFINITION_BORROW,
        storage_user: "lease",
        storage_release(value) {
          assert.equal(value, "lease");
          releases++;
        },
      },
    }),
  ).definition;
  const actor = ok(n.ta_actor_spawn(system, { definition })).actor;
  run(system);
  n.ta_definition_release(definition);
  assert.equal(releases, 0);
  ok(n.ta_actor_stop(system, actor));
  run(system);
  ok(n.ta_actor_release(system, actor));
  assert.equal(releases, 1);
  assert.equal(n.bindingStats(system).native_roots, 0);
});

test("workers have independent addon environments and can terminate with live actors", async () => {
  const { Worker } = require("node:worker_threads");
  const path = require.resolve("../lowlevel");
  const worker = new Worker(
    `
    const {parentPort}=require('node:worker_threads');const n=require(${JSON.stringify(path)});
    const {system}=n.ta_system_create(null,{});
    const {definition}=n.ta_definition_build(system,{name:'worker',data_model:'javascript',states:[{id:'idle'}]});
    n.ta_actor_spawn(system,{definition});
    n.ta_system_run(system,{now_ns:1n,budget:{work_units:100,actor_quantum:10}});
    parentPort.postMessage(n.ta_system_stats(system).actors);setInterval(()=>{},1000);
  `,
    { eval: true },
  );
  await new Promise((resolve, reject) => {
    worker.once("message", (actors) => {
      assert.equal(actors, 1);
      resolve();
    });
    worker.once("error", reject);
  });
  assert.equal(await worker.terminate(), 1);
});
