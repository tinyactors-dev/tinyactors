// staticjs.mjs: evaluate staticjs programs through staticjs.wasm, in a
// browser or in Node. A starting point to copy into an application, not a
// published package.
//
//   import { load } from './staticjs.mjs';
//   const staticjs = await load(wasmBytes, api);      // api: staticjs.api.json, parsed
//   staticjs.evaluate('[3, 1, 2].toSorted((a, b) => a - b)');   // '[1,2,3]'
//   staticjs.close();
//
// The module imports nothing and brings its own allocator. Every call takes
// its arguments in the module's memory: this wrapper places them there, with
// the struct layouts that staticjs.api.json gives for wasm32, and frees them
// after the call. The C ABI for wasm32 passes a struct by value as a pointer
// to a copy, returns one through a pointer given first, and passes a 64-bit
// integer (fuel, a seed) as a BigInt.

// SJS_LIMITS_DEFAULT in staticjs.h. Pass `limits` to load() to change any.
export const DEFAULT_LIMITS = Object.freeze({
  max_source_bytes: 1024 * 1024,
  max_token_bytes: 64 * 1024,
  max_nesting: 256,
  max_lookahead: 256,
  max_held_events: 64 * 1024,
  max_stack: 4096,
  max_frames: 256,
  max_value_items: 1024 * 1024,
  max_string_bytes: 16 * 1024 * 1024,
  max_bigint_bits: 256 * 1024,
  max_regex_states: 1024 * 1024,
});

// sjs_error_code, by value, and how a message names each.
const KINDS = ['', 'syntax', 'unsupported', 'name', 'limit', 'memory', 'fault'];
const LABELS = {
  syntax: 'syntax error', unsupported: 'unsupported', name: 'name error', limit: 'limit reached',
  memory: 'out of memory', fault: 'fault', fuel: 'out of fuel',
};

// A program refused when compiled, or an evaluation that faulted or ran out
// of fuel. `kind` is 'syntax', 'unsupported', 'name', 'limit', 'memory',
// 'fault' or 'fuel'; `line` and `column` count from 1, the column in bytes.
export class StaticJSError extends Error {
  constructor({ kind, line, column, message, subject }) {
    const where = line > 0 ? ` at ${line}:${column}` : '';
    super(`${LABELS[kind]}${where}: ${message}${subject ? ` (${subject})` : ''}`);
    this.name = 'StaticJSError';
    Object.assign(this, { kind, line, column, subject, reason: message });
  }
}

export async function load(wasm, api, { limits = {} } = {}) {
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const x = instance.exports;
  const constants = Object.fromEntries(api.constants.map((c) => [c.name, c.value]));
  const layout = (type) => api.types[type].layout.wasm32;
  const offset = (type, field) => layout(type).offsets[field];
  // Memory grows as the module allocates: take a fresh view after any call.
  const view = () => new DataView(x.memory.buffer);
  const u32 = (at) => view().getUint32(at, true);
  const setU32 = (at, value) => view().setUint32(at, value, true);
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  // Blocks placed for one call, given back when it is done.
  let blocks = [];
  const alloc = (size, align = 8) => {
    const pointer = x.sjs_wasm_alloc(size, align);
    if (pointer === 0) throw new Error('staticjs.wasm: out of memory');
    new Uint8Array(x.memory.buffer, pointer, size).fill(0);
    blocks.push([pointer, size, align]);
    return pointer;
  };
  const freeAll = () => {
    for (const [pointer, size, align] of blocks) x.sjs_wasm_free(pointer, size, align);
    blocks = [];
  };
  const struct = (type) => alloc(layout(type).size, layout(type).align);
  const setStr = (at, text) => {
    const bytes = encoder.encode(text);
    const pointer = alloc(bytes.length || 1, 1);
    new Uint8Array(x.memory.buffer, pointer, bytes.length).set(bytes);
    setU32(at + offset('sjs_str', 'ptr'), pointer);
    setU32(at + offset('sjs_str', 'length'), bytes.length);
  };
  const getStr = (at) => decoder.decode(new Uint8Array(x.memory.buffer, u32(at + offset('sjs_str', 'ptr')),
    u32(at + offset('sjs_str', 'length'))));
  // An sjs_error. Its subject is read only from the evaluator, which lives
  // on: sjs_compile has destroyed the stages a compile error's subject was in.
  const readError = (at, withSubject) => {
    const position = at + offset('sjs_error', 'at');
    return new StaticJSError({
      kind: KINDS[u32(at + offset('sjs_error', 'code'))],
      line: u32(position + offset('sjs_position', 'line')),
      column: u32(position + offset('sjs_position', 'column')),
      message: getStr(at + offset('sjs_error', 'message')),
      subject: withSubject ? getStr(at + offset('sjs_error', 'subject')) : '',
    });
  };
  const ok = (result, what) => {
    if (result !== constants.SJS_OK) throw new Error(`staticjs.wasm: ${what} failed (${result})`);
  };

  if (x.sjs_version() !== constants.SJS_VERSION) throw new Error('staticjs.wasm and staticjs.api.json differ');

  // One context, with the module's allocator, and one evaluator, for every call.
  const config = struct('sjs_context_config');
  x.sjs_wasm_allocator(config + offset('sjs_context_config', 'allocator'));
  for (const [name, value] of Object.entries({ ...DEFAULT_LIMITS, ...limits })) {
    setU32(config + offset('sjs_context_config', 'limits') + offset('sjs_limits', name), value);
  }
  const contextOut = alloc(4, 4);
  ok(x.sjs_context_init(contextOut, config), 'sjs_context_init');
  const context = u32(contextOut);
  const vmConfig = struct('sjs_vm_config');
  setU32(vmConfig + offset('sjs_vm_config', 'context'), context);
  const vmOut = alloc(4, 4);
  ok(x.sjs_vm_init(vmOut, vmConfig), 'sjs_vm_init');
  const vm = u32(vmOut);
  freeAll();

  // The value at `value` as JavaScript source, through a 4 KiB buffer.
  const write = (value) => {
    const writerConfig = struct('sjs_writer_config');
    setU32(writerConfig + offset('sjs_writer_config', 'context'), context);
    new Uint8Array(x.memory.buffer).copyWithin(writerConfig + offset('sjs_writer_config', 'value'), value,
      value + layout('sjs_value').size);
    setStr(writerConfig + offset('sjs_writer_config', 'function_text'), '<function>');
    const writerOut = alloc(4, 4);
    ok(x.sjs_writer_init(writerOut, writerConfig), 'sjs_writer_init');
    const writer = u32(writerOut);
    const capacity = 4096;
    const buffer = struct('sjs_buffer');
    const bytes = alloc(capacity, 1);
    let text = '';
    let result;
    do {
      setU32(buffer + offset('sjs_buffer', 'ptr'), bytes);
      setU32(buffer + offset('sjs_buffer', 'capacity'), capacity);
      setU32(buffer + offset('sjs_buffer', 'length'), 0);
      result = x.sjs_writer_write(writer, buffer);
      const length = u32(buffer + offset('sjs_buffer', 'length'));
      text += decoder.decode(new Uint8Array(x.memory.buffer, bytes, length), { stream: true });
    } while (result === constants.SJS_NEED_SPACE);
    x.sjs_writer_destroy(writer);
    ok(result, 'sjs_writer_write');
    return text + decoder.decode();
  };

  return {
    // Compile `source` and evaluate it; its value as JavaScript source text.
    // module: true compiles a module (without imports: this wrapper gives
    // no modules adapter), whose value is the object of its exports. `now` (milliseconds since the epoch) is the
    // clock Temporal.Now, Date.now() and new Date() read; `seed` is what
    // Math.random draws from, as `node --random-seed=<seed>` does.
    evaluate(source, { module = false, fuel = 10_000_000, now = Date.now(), seed = 1 } = {}) {
      let program = 0;
      try {
        const params = struct('sjs_compile_params');
        const compiler = params + offset('sjs_compile_params', 'compiler');
        setU32(compiler + offset('sjs_compiler_config', 'context'), context);
        setU32(compiler + offset('sjs_compiler_config', 'goal'),
          module ? constants.SJS_GOAL_MODULE : constants.SJS_GOAL_EXPRESSION);
        setStr(params + offset('sjs_compile_params', 'source'), source);
        const error = struct('sjs_error');
        setU32(params + offset('sjs_compile_params', 'error'), error);
        const programOut = alloc(4, 4);
        const compiled = x.sjs_compile(params, programOut);
        if (compiled === constants.SJS_ERR_PROGRAM) throw readError(error, false);
        ok(compiled, 'sjs_compile');
        program = u32(programOut);

        const epoch = struct('sjs_epoch');
        const ms = BigInt(Math.floor(now));
        const seconds = ms / 1000n - (ms % 1000n < 0n ? 1n : 0n);
        view().setBigInt64(epoch + offset('sjs_epoch', 'seconds'), seconds, true);
        setU32(epoch + offset('sjs_epoch', 'nanoseconds'), Number(ms - seconds * 1000n) * 1_000_000);
        ok(x.sjs_vm_set_clock(vm, epoch), 'sjs_vm_set_clock');
        ok(x.sjs_vm_set_seed(vm, BigInt(seed)), 'sjs_vm_set_seed');

        const run = struct('sjs_run_params');
        setU32(run + offset('sjs_run_params', 'program'), program);
        ok(x.sjs_vm_start(vm, run), 'sjs_vm_start');
        const status = x.sjs_vm_run(vm, BigInt(fuel));
        if (status === constants.SJS_RUN_PAUSED) {
          x.sjs_vm_reset(vm);
          throw new StaticJSError({ kind: 'fuel', line: 0, column: 0, message: `the evaluation used its ${fuel} units` });
        }
        if (status === constants.SJS_RUN_FAULT) {
          x.sjs_vm_error(vm, error);
          throw readError(error, true);
        }
        const value = struct('sjs_value');
        ok(x.sjs_vm_take_result(vm, value), 'sjs_vm_take_result');
        try {
          return write(value);
        } finally {
          x.sjs_release(context, value);
        }
      } finally {
        if (program !== 0) x.sjs_program_release(program);
        freeAll();
      }
    },

    // Give back the evaluator and the context. Memory the module took stays
    // with the module: wasm cannot shrink it.
    close() {
      x.sjs_vm_destroy(vm);
      x.sjs_context_destroy(context);
    },
  };
}
