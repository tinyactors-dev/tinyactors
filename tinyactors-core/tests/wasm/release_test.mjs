// The release's test of a wasm module (mise run release, and mise run
// wasm:test): node tests/wasm/release_test.mjs <module.wasm> <api.json>...
//
// The modules are for browsers: each is instantiated with no imports, as a
// page would. tinyactors.wasm: an address written and read back, in memory
// from the module's own allocator, many times with memory steady.
// tinyactors_runtime.wasm: that, and a chart compiled, an actor spawned,
// sent an event and run to its final state.
// tinyactors_datamodel_ecmascript.wasm: that, and an expression evaluated by
// QuickJS, its Date reading the time the host set. Struct layouts and
// constants come from the API descriptions (their wasm32 layouts), and the
// C ABI for wasm32 passes a struct by value as a pointer to a copy and
// returns one through a pointer given first.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const [modulePath, ...apiPaths] = process.argv.slice(2);
const apis = apiPaths.map((path) => JSON.parse(readFileSync(path, 'utf8')));
const types = Object.assign({}, ...apis.map((api) => api.types));
const constants = Object.fromEntries(apis.flatMap((api) => api.constants.map((c) => [c.name, c.value])));
const layout = (type) => types[type].layout.wasm32;
const offset = (type, field) => layout(type).offsets[field];

const { instance } = await WebAssembly.instantiate(readFileSync(modulePath), {});
const x = instance.exports;
if (x._initialize) x._initialize(); // the C library's constructors, once, as the web binding does
const memory = () => new DataView(x.memory.buffer);
const encoder = new TextEncoder();
const decoder = new TextDecoder();

const alloc = (size, align = 8) => {
  const pointer = x.ta_wasm_alloc(size, align);
  assert.notEqual(pointer, 0, `ta_wasm_alloc(${size}) failed`);
  new Uint8Array(x.memory.buffer, pointer, size).fill(0);
  return pointer;
};
const bytes = (text) => {
  const encoded = encoder.encode(text);
  const pointer = alloc(encoded.length || 1, 1);
  new Uint8Array(x.memory.buffer, pointer, encoded.length).set(encoded);
  return [pointer, encoded.length];
};
// A ta_string_view in memory: {pointer, length}.
const view = (text, at = alloc(layout('ta_string_view').size)) => {
  const [pointer, length] = bytes(text);
  memory().setUint32(at + offset('ta_string_view', 'pointer'), pointer, true);
  memory().setUint32(at + offset('ta_string_view', 'length'), length, true);
  return at;
};
const u32 = (at) => memory().getUint32(at, true);
const setU32 = (at, value) => memory().setUint32(at, value, true);

// tinyactors.wasm: addresses, with memory steady over many rounds.
const pages = [];
for (let round = 0; round < 500; round++) {
  const system = view('wasm');
  const buffer = alloc(constants.TA_ADDRESS_SESSION_MAX, 1);
  const out = alloc(layout('ta_buffer').size);
  setU32(out + offset('ta_buffer', 'pointer'), buffer);
  setU32(out + offset('ta_buffer', 'capacity'), constants.TA_ADDRESS_SESSION_MAX);
  assert.equal(x.ta_address_format(system, BigInt(round + 1), out), 0);
  const length = u32(out + offset('ta_buffer', 'length'));
  assert.ok(length > 0);
  const text = alloc(layout('ta_string_view').size);
  setU32(text + offset('ta_string_view', 'pointer'), buffer);
  setU32(text + offset('ta_string_view', 'length'), length);
  const address = alloc(layout('ta_address').size);
  assert.equal(x.ta_address_parse(text, address), 0);
  assert.equal(memory().getBigUint64(address + offset('ta_address', 'session'), true), BigInt(round + 1));
  if (round === 0) {
    assert.equal(decoder.decode(new Uint8Array(x.memory.buffer, buffer, length)), 'tinyactors://wasm/#_scxml_1');
  }
  for (const [pointer, size, align] of [[address, layout('ta_address').size, 8], [text, layout('ta_string_view').size, 8],
    [buffer, constants.TA_ADDRESS_SESSION_MAX, 1], [out, layout('ta_buffer').size, 8]]) x.ta_wasm_free(pointer, size, align);
  x.ta_wasm_free(u32(system + offset('ta_string_view', 'pointer')), 4, 1);
  x.ta_wasm_free(system, layout('ta_string_view').size, 8);
  pages.push(x.memory.buffer.byteLength / 65536);
}
assert.equal(pages.at(-1), pages[50], 'memory grows with every round');
const results = [`addresses round-trip 500 times, memory steady at ${pages.at(-1)} pages`];

// tinyactors_runtime.wasm: compile and run a chart.
if (x.ta_compile_charts) {
  const chart = "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' initial='idle'>" +
    "<state id='idle'><transition event='go' target='done'/></state><final id='done'/></scxml>";
  const options = alloc(layout('ta_compile_options').size);
  setU32(options + offset('ta_compile_options', 'size'), layout('ta_compile_options').size);
  x.ta_runtime_malloc(options + offset('ta_compile_options', 'alloc'));
  const source = alloc(layout('ta_source').size);
  view('smoke.scxml', source + offset('ta_source', 'name'));
  view(chart, source + offset('ta_source', 'text'));
  const result = alloc(4, 4);
  const sources = alloc(layout('ta_source_list').size);
    setU32(sources + offset('ta_source_list', 'pointer'), source);
    setU32(sources + offset('ta_source_list', 'count'), 1);
    assert.equal(x.ta_compile_charts(options, sources, result), 0, 'the chart compiles');
  const image = alloc(layout('ta_string_view').size);
  x.ta_compile_image(image, u32(result));
  const opened = alloc(layout('ta_image').size);
  const imageBytes = alloc(layout('ta_bytes').size);
    setU32(imageBytes + offset('ta_bytes', 'pointer'), u32(image + offset('ta_string_view', 'pointer')));
    setU32(imageBytes + offset('ta_bytes', 'length'), u32(image + offset('ta_string_view', 'length')));
  assert.equal(x.ta_image_open(imageBytes, opened), 0, 'the image opens');
  const config = alloc(layout('ta_system_config').size);
  setU32(config + offset('ta_system_config', 'size'), layout('ta_system_config').size);
  setU32(config + offset('ta_system_config', 'ticks_per_second'), 1000);
  setU32(config + offset('ta_system_config', 'max_actors'), 1);
  setU32(config + offset('ta_system_config', 'max_events'), 4);
  const system = alloc(4, 4);
  const setup = alloc(layout('ta_system_setup').size);
    setU32(setup + offset('ta_system_setup', 'image'), opened);
    setU32(setup + offset('ta_system_setup', 'config'), config);
    assert.equal(x.ta_runtime_system_new(setup, system), 0, 'the system starts');
  const actor = alloc(4, 4);
  const spawn = alloc(layout('ta_spawn').size);
    view('smoke', spawn + offset('ta_spawn', 'name'));
    view('', spawn + offset('ta_spawn', 'traceparent'));
    assert.equal(x.ta_system_spawn(u32(system), spawn, actor), 0, 'the actor spawns');
  const run = alloc(layout('ta_run').size);
  x.ta_runtime_run(run, system, 1000);
  const event = alloc(layout('ta_event_in').size);
  setU32(event + offset('ta_event_in', 'size'), layout('ta_event_in').size);
  setU32(event + offset('ta_event_in', 'target'), u32(actor));
  view('go', event + offset('ta_event_in', 'name'));
  assert.equal(x.ta_system_send(u32(system), event), 0, 'the event is taken');
  x.ta_runtime_run(run, system, 1000);
  const effects = alloc(4, 4);
  const count = x.ta_system_effects(u32(system), effects);
  const kinds = Array.from({ length: count },
    (_, i) => u32(u32(effects) + i * layout('ta_effect').size + offset('ta_effect', 'kind')));
  assert.ok(kinds.includes(constants.TA_EFFECT_DONE), `the actor reports done (effects ${kinds})`);
  x.ta_system_effects_ack(u32(system), count);
  x.ta_runtime_system_free(u32(system));
  x.ta_compile_free(u32(result));
  results.push('a chart compiles and runs to its final state');
}
// tinyactors_datamodel_ecmascript.wasm: QuickJS, with the time the host set.
if (x.ta_datamodel_ecmascript_new) {
  const when = 1767225600000; // 2026-01-01T00:00:00Z
  x.ta_datamodel_ecmascript_wasm_set_time(when);
  const config = alloc(layout('ta_datamodel_ecmascript_config').size);
  setU32(config + offset('ta_datamodel_ecmascript_config', 'size'), layout('ta_datamodel_ecmascript_config').size);
  const ecmascript = x.ta_datamodel_ecmascript_new(config);
  assert.notEqual(ecmascript, 0, 'the data model starts');
  const payload = x.ta_datamodel_ecmascript_payload_eval(ecmascript, 0n,
    view("({sum: [1, 2, 3].reduce((a, b) => a + b), year: new Date().getUTCFullYear(), now: Date.now()})"));
  assert.notEqual(payload, 0n, 'the expression evaluates');
  const out = alloc(layout('ta_bytes').size); // a view of the data model's memory
  assert.equal(x.ta_datamodel_ecmascript_payload_to_cbor(ecmascript, payload, out), 0, 'the value is written');
  const cbor = new Uint8Array(x.memory.buffer, u32(out + offset('ta_bytes', 'pointer')),
    u32(out + offset('ta_bytes', 'length')));
  const hex = Array.from(cbor, (b) => b.toString(16).padStart(2, '0')).join('');
  // {"sum": 6, "year": 2026, "now": 1767225600000}
  assert.equal(hex, 'a36373756d066479656172' + '1907ea' + '636e6f77' + '1b0000019b76daa800');
  x.ta_datamodel_ecmascript_free(ecmascript);
  results.push(`QuickJS evaluated h'${hex}', its clock the host's`);
}
console.log(`${modulePath.split('/').at(-1)}: ${results.join('; ')}`);
