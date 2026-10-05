'use strict';

// tinyactors.wasm as a JavaScript host uses it (mise run wasm-test, and the
// release's verify stage; root ADR 0001): the module is loaded with no
// imports, and the release's smoke test (tests/release_smoke.c) runs
// through its exports with the module's own allocator
// (ta_wasm_allocator()): a two-state chart compiled against the null data
// model, an actor spawned, `go` delivered, the actor checked to have moved
// from `idle` to `done`, and the system destroyed. The structs are laid out
// from tinyactors.api.json's wasm32 layouts, as a binding generator would.
// In the wasm32 C ABI a struct is returned through a pointer passed first,
// and a struct argument is passed by pointer. Running many times shows the
// allocator's free lists at work: after a warm-up, memory stops growing.
//
//   node tests/wasm/wasm_test.js <tinyactors.wasm> <tinyactors.api.json>

const assert = require('node:assert/strict');
const fs = require('node:fs');

const [wasmPath, apiPath] = process.argv.slice(2);
if (!wasmPath || !apiPath) {
  console.error('usage: node tests/wasm/wasm_test.js <tinyactors.wasm> <tinyactors.api.json>');
  process.exit(2);
}

const api = JSON.parse(fs.readFileSync(apiPath, 'utf8'));
const TARGET = 'wasm32-unknown-unknown';
const layout = (name) => api.types[name].layout[TARGET];
const constant = (type, name) => api.types[type].constants.find((entry) => entry.name === name).value;
const macro = (name) => BigInt(api.constants.find((entry) => entry.name === name).value);

const TA_OK = constant('ta_result', 'TA_OK');
const ATOMIC = constant('ta_scxml_state_kind', 'TA_SCXML_STATE_ATOMIC');
const EXTERNAL_TRANSITION = constant('ta_scxml_transition_kind', 'TA_SCXML_TRANSITION_EXTERNAL');
const EXTERNAL_EVENT = constant('ta_event_type', 'TA_EVENT_EXTERNAL');
const COMPLETED = constant('ta_input_outcome_kind', 'TA_INPUT_COMPLETED');
const INVOKE_ID_NONE = macro('TA_INVOKE_ID_NONE');

async function main() {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {});
  const ta = instance.exports;
  const view = () => new DataView(ta.memory.buffer);
  const blocks = [];
  // A zeroed block from the module's allocator, freed at the end of a run.
  const alloc = (size, align) => {
    const pointer = ta.ta_wasm_alloc(size, align);
    assert.notEqual(pointer, 0, `ta_wasm_alloc(${size}, ${align})`);
    new Uint8Array(ta.memory.buffer, pointer, size).fill(0);
    blocks.push([pointer, size, align]);
    return pointer;
  };
  const struct = (name) => {
    const { size, align } = layout(name);
    return alloc(size, align);
  };
  const u32 = (at, value) => view().setUint32(at, value, true);
  const u64 = (at, value) => view().setBigUint64(at, BigInt(value), true);
  const getU32 = (at) => view().getUint32(at, true);
  const getU64 = (at) => view().getBigUint64(at, true);
  // A ta_str at `at` naming a copy of `text` in linear memory.
  const str = (at, text) => {
    const bytes = new TextEncoder().encode(text);
    const pointer = alloc(Math.max(bytes.length, 1), 1);
    new Uint8Array(ta.memory.buffer, pointer, bytes.length).set(bytes);
    u32(at + layout('ta_str').offsets.ptr, pointer);
    u32(at + layout('ta_str').offsets.len, bytes.length);
  };
  // A zero-terminated array of `count` elements of `name`.
  const array = (name, count) => {
    const { size, align } = layout(name);
    return alloc(size * (count + 1), align);
  };

  function run() {
    const allocator = struct('ta_allocator');
    ta.ta_wasm_allocator(allocator);
    const config = struct('ta_system_config');
    const allocatorSize = layout('ta_allocator').size;
    new Uint8Array(ta.memory.buffer, config + layout('ta_system_config').offsets.allocator, allocatorSize)
      .set(new Uint8Array(ta.memory.buffer, allocator, allocatorSize));
    const systemOut = alloc(4, 4);
    assert.equal(ta.ta_system_create(config, systemOut), TA_OK, 'ta_system_create');
    const system = getU32(systemOut);

    // The chart: idle --go--> done, against the null data model.
    const strSize = layout('ta_str').size;
    const targets = array('ta_str', 1);
    str(targets, 'done');
    const transition = layout('ta_scxml_transition_spec');
    const transitions = array('ta_scxml_transition_spec', 1);
    u32(transitions + transition.offsets.kind, EXTERNAL_TRANSITION);
    str(transitions + transition.offsets.event, 'go');
    u32(transitions + transition.offsets.targets, targets);
    const state = layout('ta_scxml_state_spec');
    const states = array('ta_scxml_state_spec', 2);
    u32(states + state.offsets.kind, ATOMIC);
    str(states + state.offsets.id, 'idle');
    u32(states + state.offsets.transitions, transitions);
    u32(states + state.size + state.offsets.kind, ATOMIC);
    str(states + state.size + state.offsets.id, 'done');
    const specLayout = layout('ta_scxml_spec');
    const spec = struct('ta_scxml_spec');
    str(spec + specLayout.offsets.name, 'smoke');
    str(spec + specLayout.offsets.datamodel, 'null');
    u32(spec + specLayout.offsets.states, states);

    const compileLayout = layout('ta_definition_compile_params');
    const compile = struct('ta_definition_compile_params');
    u32(compile + compileLayout.offsets.spec, spec);
    ta.ta_null_datamodel(compile + compileLayout.offsets.datamodel);
    new Uint8Array(ta.memory.buffer, compile + compileLayout.offsets.allocator, allocatorSize)
      .set(new Uint8Array(ta.memory.buffer, allocator, allocatorSize));
    const definitionOut = alloc(4, 4);
    u32(compile + compileLayout.offsets.out, definitionOut);
    assert.equal(ta.ta_definition_compile_spec(system, compile), TA_OK, 'the chart compiles');
    const idOut = alloc(8, 8);
    assert.equal(ta.ta_system_register_definition(system, getU32(definitionOut), idOut), TA_OK,
      'ta_system_register_definition');
    const definition = getU64(idOut);

    const spawnLayout = layout('ta_actor_spawn_params');
    const spawn = struct('ta_actor_spawn_params');
    u64(spawn + spawnLayout.offsets.definition, definition);
    const actorOut = alloc(8, 8);
    u32(spawn + spawnLayout.offsets.out, actorOut);
    assert.equal(ta.ta_actor_spawn(system, spawn), TA_OK, 'ta_actor_spawn');
    const actor = getU64(actorOut);

    // One ta_run with room for one outcome: the kind of the outcome it reports.
    const runLayout = layout('ta_run_params');
    const outcomeLayout = layout('ta_input_outcome');
    const statusOut = alloc(4, 4);
    const countOut = alloc(4, 4);
    const outcome = alloc(outcomeLayout.size, outcomeLayout.align);
    const runOnce = () => {
      const params = struct('ta_run_params');
      u32(params + runLayout.offsets.budget, 100);
      u32(params + runLayout.offsets.outcomes, outcome);
      u32(params + runLayout.offsets.outcome_capacity, 1);
      u32(params + runLayout.offsets.outcome_count, countOut);
      u32(params + runLayout.offsets.status, statusOut);
      assert.equal(ta.ta_run(system, params), TA_OK, 'ta_run');
      assert.equal(getU32(countOut), 1, 'ta_run reports one outcome');
      return getU32(outcome + outcomeLayout.offsets.kind);
    };
    // Whether the state named `name` is active.
    const active = (name) => {
      const findLayout = layout('ta_definition_find_state_params');
      const find = struct('ta_definition_find_state_params');
      u64(find + findLayout.offsets.definition, definition);
      str(find + findLayout.offsets.id, name);
      const stateOut = alloc(4, 4);
      u32(find + findLayout.offsets.out, stateOut);
      assert.equal(ta.ta_definition_find_state(system, find), TA_OK, `the chart has ${name}`);
      const statesLayout = layout('ta_actor_active_states_params');
      const query = struct('ta_actor_active_states_params');
      const list = alloc(4 * 8, 4);
      const countOut = alloc(4, 4);
      u64(query + statesLayout.offsets.actor, actor);
      u32(query + statesLayout.offsets.states, list);
      u32(query + statesLayout.offsets.capacity, 8);
      u32(query + statesLayout.offsets.count, countOut);
      assert.equal(ta.ta_actor_active_states(system, query), TA_OK, 'ta_actor_active_states');
      const wanted = getU32(stateOut);
      for (let index = 0; index < getU32(countOut); index++) {
        if (getU32(list + 4 * index) === wanted) return true;
      }
      return false;
    };

    assert.equal(runOnce(), COMPLETED, 'the bootstrap completes');
    assert.ok(active('idle'), 'the actor starts in idle');

    const text = alloc(strSize, layout('ta_str').align);
    str(text, 'go');
    const atomOut = alloc(4, 4);
    assert.equal(ta.ta_system_intern_atom(system, text, atomOut), TA_OK, 'ta_system_intern_atom');
    const segmentsLayout = layout('ta_atom_list');
    const segments = struct('ta_atom_list');
    u32(segments + segmentsLayout.offsets.atoms, atomOut);
    u32(segments + segmentsLayout.offsets.count, 1);
    const nameOut = alloc(4, 4);
    assert.equal(ta.ta_event_name_intern(system, segments, nameOut), TA_OK, 'ta_event_name_intern');

    const eventLayout = layout('ta_event');
    const event = struct('ta_event');
    u32(event + eventLayout.offsets.name, getU32(nameOut));
    u32(event + eventLayout.offsets.type, EXTERNAL_EVENT);
    u64(event + eventLayout.offsets.invoke_id, INVOKE_ID_NONE);
    const deliverLayout = layout('ta_actor_deliver_params');
    const deliver = struct('ta_actor_deliver_params');
    u64(deliver + deliverLayout.offsets.actor, actor);
    u32(deliver + deliverLayout.offsets.event, event);
    assert.equal(ta.ta_actor_deliver(system, deliver), TA_OK, 'ta_actor_deliver');
    assert.equal(runOnce(), COMPLETED, 'the macrostep ends stable');
    assert.ok(!active('idle') && active('done'), 'go moves the actor from idle to done');

    ta.ta_system_destroy(system);
    while (blocks.length > 0) {
      const [pointer, size, align] = blocks.pop();
      ta.ta_wasm_free(pointer, size, align);
    }
  }

  const version = ta.ta_version();
  for (let i = 0; i < 20; i++) run();
  const warm = ta.memory.buffer.byteLength;
  const runs = 500;
  for (let i = 0; i < runs; i++) run();
  const pages = ta.memory.buffer.byteLength / 65536;
  assert.equal(ta.memory.buffer.byteLength, warm, 'memory stops growing: the free lists reuse blocks');
  console.log(`tinyactors ${version >> 16}.${(version >> 8) & 0xff}.${version & 0xff}: idle -> done on go ` +
    `${runs + 20} times through ta_wasm_allocator(); memory stays at ${pages} pages after the warm-up`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
