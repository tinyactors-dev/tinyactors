// The release's test of staticjs.wasm (mise run release, and mise run
// wasm-test): node tests/wasm/release_test.mjs <staticjs.wasm> <staticjs.api.json> <staticjs.h>
//
// The module is for browsers: it is instantiated with no imports, as a page
// would. The test checks that it imports nothing and exports memory and
// every function the API description says it implements, then evaluates a
// program through it, many times, in memory from the module's own allocator:
// compiled, run on fuel pushed a little at a time, its result written as
// text, and every allocation given back, with memory steady over the rounds.
//
// Struct layouts and constants come from the API description (their wasm32
// layouts); the default limits from SJS_LIMITS_DEFAULT in the header. The C
// ABI for wasm32 passes a struct by value as a pointer to a copy, and
// returns one through a pointer given first.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const [modulePath, apiPath, headerPath] = process.argv.slice(2);
const api = JSON.parse(readFileSync(apiPath, 'utf8'));
const constants = Object.fromEntries(api.constants.map((c) => [c.name, c.value]));
const layout = (type) => api.types[type].layout.wasm32;
const offset = (type, field) => layout(type).offsets[field];

const module = await WebAssembly.compile(readFileSync(modulePath));
assert.deepEqual(WebAssembly.Module.imports(module), [], 'the module imports nothing');
const exported = new Map(WebAssembly.Module.exports(module).map((e) => [e.name, e.kind]));
assert.equal(exported.get('memory'), 'memory', 'the module exports its memory');
const functions = api.functions.filter((f) => f.public && f.implemented && (!f.platform || f.platform === 'wasm'));
for (const f of functions) assert.equal(exported.get(f.name), 'function', `the module exports ${f.name}`);

const instance = await WebAssembly.instantiate(module, {});
const x = instance.exports;
const memory = () => new DataView(x.memory.buffer);
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const u32 = (at) => memory().getUint32(at, true);
const setU32 = (at, value) => memory().setUint32(at, value, true);

// The default limits, as the header gives them.
const header = readFileSync(headerPath, 'utf8');
const defaults = header.match(/#define SJS_LIMITS_DEFAULT[\s\S]*?\}\)/)[0];
const limits = Object.fromEntries([...defaults.matchAll(/\.(\w+) = ([\du *]+?)\s*[,}]/g)]
  .map(([, name, value]) => [name, value.replace(/u/g, '').split('*').reduce((a, b) => a * Number(b), 1)]));
assert.equal(Object.keys(limits).length, api.types.sjs_limits.fields.length, 'every limit has a default');

const blocks = [];
const alloc = (size, align = 8) => {
  const pointer = x.sjs_wasm_alloc(size, align);
  assert.notEqual(pointer, 0, `sjs_wasm_alloc(${size}) failed`);
  new Uint8Array(x.memory.buffer, pointer, size).fill(0);
  blocks.push([pointer, size, align]);
  return pointer;
};
const freeAll = () => {
  for (const [pointer, size, align] of blocks.splice(0)) x.sjs_wasm_free(pointer, size, align);
};
// An sjs_str at `at`: {ptr, length}, its bytes in the module's memory.
const str = (at, text) => {
  const encoded = encoder.encode(text);
  const pointer = alloc(encoded.length || 1, 1);
  new Uint8Array(x.memory.buffer, pointer, encoded.length).set(encoded);
  setU32(at + offset('sjs_str', 'ptr'), pointer);
  setU32(at + offset('sjs_str', 'length'), encoded.length);
};

assert.equal(x.sjs_version(), constants.SJS_VERSION, 'the module is the header version');

const source = '({ total: [1, 2, 3, 4, 5].map((n) => n * n).reduce((sum, n) => sum + n, 0),' +
  ' name: `static${"js"}`, big: 2n ** 70n })';
const expected = '{"total":55,"name":"staticjs","big":1180591620717411303424n}';
const ok = constants.SJS_OK;
const pages = [];
let fuel = 0n;
let pauses = 0;
for (let round = 0; round < 200; round++) {
  // A context with the module's allocator and the default limits.
  const config = alloc(layout('sjs_context_config').size);
  x.sjs_wasm_allocator(config + offset('sjs_context_config', 'allocator'));
  const at = config + offset('sjs_context_config', 'limits');
  for (const [name, value] of Object.entries(limits)) setU32(at + offset('sjs_limits', name), value);
  const contextOut = alloc(4, 4);
  assert.equal(x.sjs_context_init(contextOut, config), ok, 'the context starts');
  const context = u32(contextOut);

  // Compiled as an expression.
  const params = alloc(layout('sjs_compile_params').size);
  const compiler = params + offset('sjs_compile_params', 'compiler');
  setU32(compiler + offset('sjs_compiler_config', 'context'), context);
  setU32(compiler + offset('sjs_compiler_config', 'goal'), constants.SJS_GOAL_EXPRESSION);
  str(params + offset('sjs_compile_params', 'source'), source);
  const programOut = alloc(4, 4);
  assert.equal(x.sjs_compile(params, programOut), ok, 'the program compiles');
  const program = u32(programOut);

  // Run on 16 units of fuel at a time.
  const vmConfig = alloc(layout('sjs_vm_config').size);
  setU32(vmConfig + offset('sjs_vm_config', 'context'), context);
  const vmOut = alloc(4, 4);
  assert.equal(x.sjs_vm_init(vmOut, vmConfig), ok, 'the evaluator starts');
  const vm = u32(vmOut);
  const run = alloc(layout('sjs_run_params').size);
  setU32(run + offset('sjs_run_params', 'program'), program);
  assert.equal(x.sjs_vm_start(vm, run), ok, 'the evaluation starts');
  let status;
  pauses = 0;
  while ((status = x.sjs_vm_run(vm, 16n)) === constants.SJS_RUN_PAUSED) {
    pauses++;
    assert.ok(x.sjs_vm_fuel_used(vm) < 100000n, 'the program finishes on its fuel budget');
  }
  assert.equal(status, constants.SJS_RUN_DONE, 'the program finishes');
  fuel = x.sjs_vm_fuel_used(vm);
  const value = alloc(layout('sjs_value').size);
  assert.equal(x.sjs_vm_take_result(vm, value), ok, 'the result is taken');

  // Written as text.
  const writerConfig = alloc(layout('sjs_writer_config').size);
  setU32(writerConfig + offset('sjs_writer_config', 'context'), context);
  new Uint8Array(x.memory.buffer).copyWithin(writerConfig + offset('sjs_writer_config', 'value'), value,
    value + layout('sjs_value').size);
  const writerOut = alloc(4, 4);
  assert.equal(x.sjs_writer_init(writerOut, writerConfig), ok, 'the writer starts');
  const capacity = 256;
  const buffer = alloc(layout('sjs_buffer').size);
  setU32(buffer + offset('sjs_buffer', 'ptr'), alloc(capacity, 1));
  setU32(buffer + offset('sjs_buffer', 'capacity'), capacity);
  assert.equal(x.sjs_writer_write(u32(writerOut), buffer), ok, 'the result is written');
  const text = decoder.decode(new Uint8Array(x.memory.buffer, u32(buffer + offset('sjs_buffer', 'ptr')),
    u32(buffer + offset('sjs_buffer', 'length'))));
  assert.equal(text, expected);

  x.sjs_writer_destroy(u32(writerOut));
  x.sjs_release(context, value);
  x.sjs_vm_destroy(vm);
  x.sjs_program_release(program);
  assert.equal(x.sjs_context_live_bytes(context), 0, 'the context gives back every allocation');
  x.sjs_context_destroy(context);
  freeAll();
  pages.push(x.memory.buffer.byteLength / 65536);
}
assert.equal(pages.at(-1), pages[20], 'memory grows with every round');
console.log(`${modulePath.split('/').at(-1)}: imports nothing, exports ${functions.length} functions and memory; ` +
  `${expected} 200 times in ${fuel} fuel, paused ${pauses} times each, memory steady at ${pages.at(-1)} pages`);
