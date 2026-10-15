// staticjs.mjs: evaluate staticjs programs through staticjs.wasm, in a
// browser or in Node. A starting point to copy into an application, not a
// published package.
//
//   import { load } from './staticjs.mjs';
//   const staticjs = await load(wasmBytes, api);      // api: staticjs.api.json, parsed
//   const context = staticjs.createContext();
//   context.evaluate('[3, 1, 2].toSorted((a, b) => a - b)');            // [1, 2, 3]
//
//   const total = context.compile('order.items.reduce((sum, i) => sum + i.price * i.count, 0)',
//     { globals: ['order'] });
//   total.evaluate({ order: { items: [{ price: 2.5, count: 2 }] } });   // 5
//
//   const evaluation = total.start({ order }, { output: 'json' });
//   while (evaluation.run(10_000) === 'paused') await new Promise((resolve) => setTimeout(resolve));
//   evaluation.result;                                                  // '5'
//
//   total.release();
//   context.close();
//
// The module imports nothing and brings its own allocator. Every call takes
// its arguments in the module's memory: this wrapper places them there, with
// the struct layouts that staticjs.api.json gives for wasm32, and frees them
// after the call. The C ABI for wasm32 passes a struct by value as a pointer
// to a copy, returns one through a pointer given first, and passes a 64-bit
// integer (fuel, a seed) as a BigInt.

// SJS_LIMITS_DEFAULT in staticjs.h. Pass `limits` to createContext() to change any.
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

// A program refused when compiled, an evaluation that faulted or ran out of
// fuel, or a global the context could not hold. `kind` is 'syntax',
// 'unsupported', 'name', 'limit', 'memory', 'fault' or 'fuel'; `line` and
// `column` count from 1, the column in bytes.
export class StaticJSError extends Error {
  constructor({ kind, line = 0, column = 0, message, subject = '' }) {
    const where = line > 0 ? ` at ${line}:${column}` : '';
    super(`${LABELS[kind]}${where}: ${message}${subject ? ` (${subject})` : ''}`);
    this.name = 'StaticJSError';
    Object.assign(this, { kind, line, column, subject, reason: message });
  }
}

// What an evaluation's result is given as: the value as a JavaScript value,
// as JSON text, or as JavaScript source text that evaluates to it.
const OUTPUTS = ['value', 'json', 'source'];

export async function load(wasm, api) {
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const x = instance.exports;
  const constants = Object.fromEntries(api.constants.map((c) => [c.name, c.value]));
  const layout = (type) => api.types[type].layout.wasm32;
  const offset = (type, field) => layout(type).offsets[field];
  const VALUE = layout('sjs_value').size;
  // Memory grows as the module allocates: take a fresh view after any call.
  const view = () => new DataView(x.memory.buffer);
  const u32 = (at) => view().getUint32(at, true);
  const setU32 = (at, value) => view().setUint32(at, value, true);
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  if (x.sjs_version() !== constants.SJS_VERSION) throw new Error('staticjs.wasm and staticjs.api.json differ');

  // Blocks of the module's memory, given back together: one call's
  // arguments, or what an evaluation holds until it ends.
  const blocks = () => {
    const held = [];
    const alloc = (size, align = 8) => {
      const pointer = x.sjs_wasm_alloc(size, align);
      if (pointer === 0) throw new StaticJSError({ kind: 'memory', message: 'staticjs.wasm has no memory left' });
      new Uint8Array(x.memory.buffer, pointer, size).fill(0);
      held.push([pointer, size, align]);
      return pointer;
    };
    alloc.struct = (type) => alloc(layout(type).size, layout(type).align);
    // An sjs_str at `at`, its bytes placed beside it.
    alloc.str = (at, text) => {
      const bytes = encoder.encode(text);
      const pointer = alloc(bytes.length || 1, 1);
      new Uint8Array(x.memory.buffer, pointer, bytes.length).set(bytes);
      setU32(at + offset('sjs_str', 'ptr'), pointer);
      setU32(at + offset('sjs_str', 'length'), bytes.length);
      return at;
    };
    alloc.free = () => {
      for (const [pointer, size, align] of held.splice(0)) x.sjs_wasm_free(pointer, size, align);
    };
    return alloc;
  };
  const getStr = (at) => decoder.decode(new Uint8Array(x.memory.buffer, u32(at + offset('sjs_str', 'ptr')),
    u32(at + offset('sjs_str', 'length'))));
  const ok = (result, what) => {
    if (result === constants.SJS_OK) return;
    if (result === constants.SJS_ERR_LIMIT) throw new StaticJSError({ kind: 'limit', message: `${what}: a limit of the context` });
    if (result === constants.SJS_ERR_NOMEM) throw new StaticJSError({ kind: 'memory', message: what });
    throw new Error(`staticjs.wasm: ${what} failed (${result})`);
  };
  // An sjs_error. Its subject is read only from an evaluator, which lives
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

  // Make an sjs_value at `out` from a JavaScript value, in `context`: the
  // caller owns it. undefined, null, booleans, numbers, strings, BigInts,
  // arrays, plain objects, Maps and Sets; anything else is a TypeError.
  const toValue = (context, input, out) => {
    const at = blocks();
    try {
      switch (typeof input) {
        case 'undefined': x.sjs_undefined(out); return;
        case 'boolean': x.sjs_boolean(out, input ? 1 : 0); return;
        case 'number': x.sjs_number(out, input); return;
        case 'string': ok(x.sjs_string_new(context, at.str(at.struct('sjs_str'), input), out), 'a string'); return;
        case 'bigint': ok(x.sjs_bigint_new(context, at.str(at.struct('sjs_str'), String(input)), out), 'a BigInt'); return;
        case 'object': break;
        default: throw new TypeError(`staticjs has no value for a ${typeof input}`);
      }
      if (input === null) { x.sjs_null(out); return; }
      const prototype = Object.getPrototypeOf(input);
      let items;
      let add;
      if (Array.isArray(input)) {
        ok(x.sjs_array_new(context, input.length, out), 'an array');
        items = Array.from(input, (item) => [item]);
        add = (slot) => x.sjs_array_append(context, out, slot);
      } else if (input instanceof Set) {
        ok(x.sjs_set_new(context, input.size, out), 'a Set');
        items = [...input].map((item) => [item]);
        add = (slot) => x.sjs_set_add(context, out, slot);
      } else if (input instanceof Map) {
        ok(x.sjs_map_new(context, input.size, out), 'a Map');
        items = [...input];
        add = (slot) => x.sjs_map_set(context, out, slot);
      } else if (prototype === Object.prototype || prototype === null) {
        const entries = Object.entries(input);
        ok(x.sjs_object_new(context, entries.length, out), 'an object');
        items = entries.map(([key, value]) => [value, key]);
        add = (slot) => x.sjs_object_put(context, out, slot);
      } else {
        throw new TypeError(`staticjs has no value for a ${input.constructor?.name ?? 'object'}`);
      }
      try {
        // An array's or a set's element, a map's entry or an object's property.
        const slot = input instanceof Map ? at.struct('sjs_map_entry') : at.struct('sjs_property');
        const valueAt = slot + (input instanceof Map ? offset('sjs_map_entry', 'value')
          : Array.isArray(input) || input instanceof Set ? 0 : offset('sjs_property', 'value'));
        for (const item of items) {
          if (input instanceof Map) {
            toValue(context, item[0], slot + offset('sjs_map_entry', 'key'));
            toValue(context, item[1], valueAt);
          } else {
            if (item.length === 2) at.str(slot + offset('sjs_property', 'key'), item[1]);
            toValue(context, item[0], valueAt);
          }
          const result = add(slot);
          if (result !== constants.SJS_OK) {
            // The container did not take them.
            if (input instanceof Map) x.sjs_release(context, slot + offset('sjs_map_entry', 'key'));
            x.sjs_release(context, valueAt);
            if (result === constants.SJS_ERR_INVALID) {
              throw new TypeError(input instanceof Map ? 'a Map\'s key in staticjs is a primitive'
                : input instanceof Set ? 'a Set\'s element in staticjs is a primitive'
                  : `staticjs has no object with the key ${item[1]}`);
            }
            ok(result, 'an element');
          }
        }
      } catch (error) {
        x.sjs_release(context, out);
        throw error;
      }
    } finally {
      at.free();
    }
  };

  // The value at `value` as JavaScript source, through a 4 KiB buffer.
  const write = (context, value) => {
    const at = blocks();
    try {
      const config = at.struct('sjs_writer_config');
      setU32(config + offset('sjs_writer_config', 'context'), context);
      new Uint8Array(x.memory.buffer).copyWithin(config + offset('sjs_writer_config', 'value'), value, value + VALUE);
      at.str(config + offset('sjs_writer_config', 'function_text'), '<function>');
      const writerOut = at(4, 4);
      ok(x.sjs_writer_init(writerOut, config), 'sjs_writer_init');
      const writer = u32(writerOut);
      const capacity = 4096;
      const buffer = at.struct('sjs_buffer');
      const bytes = at(capacity, 1);
      let text = '';
      let result;
      do {
        setU32(buffer + offset('sjs_buffer', 'ptr'), bytes);
        setU32(buffer + offset('sjs_buffer', 'capacity'), capacity);
        setU32(buffer + offset('sjs_buffer', 'length'), 0);
        result = x.sjs_writer_write(writer, buffer);
        text += decoder.decode(new Uint8Array(x.memory.buffer, bytes, u32(buffer + offset('sjs_buffer', 'length'))),
          { stream: true });
      } while (result === constants.SJS_NEED_SPACE);
      x.sjs_writer_destroy(writer);
      ok(result, 'sjs_writer_write');
      return text + decoder.decode();
    } finally {
      at.free();
    }
  };

  // The value at `value` as a JavaScript value, read through the value API.
  // A regular expression becomes a RegExp, a Date a Date, and a Temporal
  // value one of the host's Temporal (a TypeError where the host has none).
  // An array gives its elements only, as JSON does: a match result's
  // `index` and `groups` stay behind. An iterator or a function is a
  // TypeError. With `json`, everything is as JSON.stringify sees it: a
  // Temporal value is its string, an iterator {}, and a function undefined.
  // `budget.left` is the values it may still make: values share parts, so a
  // small result can stand for a tree far larger than the fuel it took.
  const fromValue = (context, value, json, budget) => {
    if (--budget.left < 0) {
      throw new StaticJSError({ kind: 'limit', message: `the result is more than ${budget.items} values` });
    }
    const kind = view().getUint8(value + offset('sjs_value', 'kind'));
    const as = value + offset('sjs_value', 'as');
    switch (kind) {
      case constants.SJS_UNDEFINED: return undefined;
      case constants.SJS_NULL: return null;
      case constants.SJS_BOOLEAN: return u32(value + offset('sjs_value', 'boolean')) !== 0;
      case constants.SJS_NUMBER: return view().getFloat64(as, true);
      case constants.SJS_FUNCTION:
        if (json) return undefined;
        throw new TypeError('a function cannot leave staticjs: return what it computes');
      case constants.SJS_ITERATOR:
        if (json) return {};
        throw new TypeError('an iterator cannot leave staticjs: return [...iterator]');
      case constants.SJS_REGEXP: {
        const text = write(context, value);
        const end = text.lastIndexOf('/');
        return new RegExp(text.slice(1, end), text.slice(end + 1));
      }
      case constants.SJS_TEMPORAL: {
        const text = write(context, value);
        const date = /^new Date\((.*)\)$/.exec(text);
        if (date) return new Date(date[1] === 'NaN' ? NaN : JSON.parse(date[1]));
        const [, type, argument] = /^Temporal\.(\w+)\.from\((.*)\)$/.exec(text);
        if (json) return JSON.parse(argument);
        if (globalThis.Temporal === undefined) throw new TypeError(`this JavaScript has no Temporal for ${text}`);
        return globalThis.Temporal[type].from(JSON.parse(argument));
      }
      default: break;
    }
    const at = blocks();
    try {
      switch (kind) {
        case constants.SJS_STRING: {
          const text = at.struct('sjs_str');
          x.sjs_string_text(text, value);
          return getStr(text);
        }
        case constants.SJS_BIGINT: {
          const buffer = at.struct('sjs_buffer');
          const capacity = x.sjs_bigint_text_bound(value);
          setU32(buffer + offset('sjs_buffer', 'ptr'), at(capacity, 1));
          setU32(buffer + offset('sjs_buffer', 'capacity'), capacity);
          ok(x.sjs_bigint_text(context, value, buffer), 'sjs_bigint_text');
          return BigInt(decoder.decode(new Uint8Array(x.memory.buffer, u32(buffer + offset('sjs_buffer', 'ptr')),
            u32(buffer + offset('sjs_buffer', 'length')))));
        }
        case constants.SJS_ARRAY:
        case constants.SJS_SET: {
          const slot = at(VALUE);
          const items = [];
          for (let i = 0, n = x.sjs_length(value); i < n; i++) {
            if (kind === constants.SJS_ARRAY) x.sjs_array_at(slot, value, i);
            else x.sjs_set_at(slot, value, i);
            items.push(fromValue(context, slot, json, budget));
          }
          return kind === constants.SJS_ARRAY ? items : new Set(items);
        }
        case constants.SJS_MAP: {
          const entry = at.struct('sjs_map_entry');
          const map = new Map();
          for (let i = 0, n = x.sjs_map_size(value); i < n; i++) {
            ok(x.sjs_map_at(value, i, entry), 'sjs_map_at');
            map.set(fromValue(context, entry + offset('sjs_map_entry', 'key'), json, budget),
              fromValue(context, entry + offset('sjs_map_entry', 'value'), json, budget));
          }
          return map;
        }
        default: { // SJS_OBJECT
          const property = at.struct('sjs_property');
          const object = {};
          for (let i = 0, n = x.sjs_length(value); i < n; i++) {
            ok(x.sjs_object_at(value, i, property), 'sjs_object_at');
            Object.defineProperty(object, getStr(property + offset('sjs_property', 'key')), {
              value: fromValue(context, property + offset('sjs_property', 'value'), json, budget),
              enumerable: true, writable: true, configurable: true,
            });
          }
          return object;
        }
      }
    } finally {
      at.free();
    }
  };

  // The result at `value` in the output asked for, of at most `items` values.
  const output = (context, value, { kind, items }) => {
    if (kind === 'source') return write(context, value);
    const budget = { items, left: items };
    if (kind === 'json') return JSON.stringify(fromValue(context, value, true, budget));
    return fromValue(context, value, false, budget);
  };

  return {
    // A context: its own limits, and the values, programs and evaluations
    // made in it. Contexts share nothing but the module's memory.
    createContext({ limits = {} } = {}) {
      // A result read as a value or as JSON makes at most max_value_items values.
      const items = { ...DEFAULT_LIMITS, ...limits }.max_value_items;
      const at = blocks();
      let context;
      try {
        const config = at.struct('sjs_context_config');
        x.sjs_wasm_allocator(config + offset('sjs_context_config', 'allocator'));
        for (const [name, value] of Object.entries({ ...DEFAULT_LIMITS, ...limits })) {
          if (!(name in DEFAULT_LIMITS)) throw new TypeError(`no limit is called ${name}`);
          setU32(config + offset('sjs_context_config', 'limits') + offset('sjs_limits', name), value);
        }
        const out = at(4, 4);
        ok(x.sjs_context_init(out, config), 'sjs_context_init');
        context = u32(out);
      } finally {
        at.free();
      }
      // Programs not yet released, and evaluations not yet ended.
      const live = new Set();

      const compile = (source, { module = false, globals = [] } = {}) => {
        if (context === 0) throw new Error('the context is closed');
        const call = blocks();
        let program;
        try {
          const params = call.struct('sjs_compile_params');
          const compiler = params + offset('sjs_compile_params', 'compiler');
          setU32(compiler + offset('sjs_compiler_config', 'context'), context);
          setU32(compiler + offset('sjs_compiler_config', 'goal'),
            module ? constants.SJS_GOAL_MODULE : constants.SJS_GOAL_EXPRESSION);
          const names = call(Math.max(globals.length, 1) * layout('sjs_str').size, layout('sjs_str').align);
          globals.forEach((name, i) => call.str(names + i * layout('sjs_str').size, name));
          setU32(compiler + offset('sjs_compiler_config', 'globals'), names);
          setU32(compiler + offset('sjs_compiler_config', 'global_count'), globals.length);
          call.str(params + offset('sjs_compile_params', 'source'), source);
          const error = call.struct('sjs_error');
          setU32(params + offset('sjs_compile_params', 'error'), error);
          const out = call(4, 4);
          const compiled = x.sjs_compile(params, out);
          if (compiled === constants.SJS_ERR_PROGRAM) throw readError(error, false);
          ok(compiled, 'sjs_compile');
          program = u32(out);
        } finally {
          call.free();
        }
        const names = [...globals];

        // An evaluation of the program, run on the fuel run() is given.
        const start = (values = {}, { now = Date.now(), seed = 1, output: kind = 'value' } = {}) => {
          if (program === 0) throw new Error('the program is released');
          if (!OUTPUTS.includes(kind)) throw new TypeError(`output is one of ${OUTPUTS.join(', ')}`);
          const own = program;     // retained until the evaluation ends
          const held = blocks();   // until the evaluation ends
          const globalValues = held(Math.max(names.length, 1) * VALUE);
          let made = 0;
          let vm = 0;
          const end = () => {
            if (vm !== 0) x.sjs_vm_destroy(vm);
            for (let i = 0; i < made; i++) x.sjs_release(context, globalValues + i * VALUE);
            held.free();
            vm = 0;
            made = 0;
            live.delete(evaluation);
            x.sjs_program_release(own);
          };
          const evaluation = {
            state: 'paused',
            result: undefined,
            // Run on `fuel` more units: 'paused' when they are spent,
            // 'done' with `result` set. A fault throws a StaticJSError
            // and ends the evaluation.
            run(fuel) {
              if (evaluation.state !== 'paused') throw new Error(`the evaluation is ${evaluation.state}`);
              const status = x.sjs_vm_run(vm, BigInt(fuel));
              evaluation.fuelUsed = Number(x.sjs_vm_fuel_used(vm));
              if (status === constants.SJS_RUN_PAUSED) return 'paused';
              // Done or faulted: the evaluation ends here, whatever happens below.
              evaluation.state = status === constants.SJS_RUN_FAULT ? 'faulted' : 'done';
              const call = blocks();
              try {
                if (status === constants.SJS_RUN_FAULT) {
                  const error = call.struct('sjs_error');
                  x.sjs_vm_error(vm, error);
                  throw readError(error, true);
                }
                const value = call(VALUE);
                ok(x.sjs_vm_take_result(vm, value), 'sjs_vm_take_result');
                try {
                  evaluation.result = output(context, value, { kind, items });
                } finally {
                  x.sjs_release(context, value);
                }
                return 'done';
              } finally {
                call.free();
                end();
              }
            },
            // Abandon a paused evaluation and release what it holds.
            cancel() {
              if (evaluation.state !== 'paused') return;
              evaluation.state = 'cancelled';
              end();
            },
            fuelUsed: 0,
          };
          x.sjs_program_retain(own);
          live.add(evaluation);
          try {
            names.forEach((name) => {
              if (!(name in values)) throw new TypeError(`no value for the global ${name}`);
              toValue(context, values[name], globalValues + made * VALUE);
              made++;
            });
            const call = blocks();
            try {
              const config = call.struct('sjs_vm_config');
              setU32(config + offset('sjs_vm_config', 'context'), context);
              const out = call(4, 4);
              ok(x.sjs_vm_init(out, config), 'sjs_vm_init');
              vm = u32(out);
              const epoch = call.struct('sjs_epoch');
              const ms = BigInt(Math.floor(now));
              const seconds = ms / 1000n - (ms % 1000n < 0n ? 1n : 0n);
              view().setBigInt64(epoch + offset('sjs_epoch', 'seconds'), seconds, true);
              setU32(epoch + offset('sjs_epoch', 'nanoseconds'), Number(ms - seconds * 1000n) * 1_000_000);
              ok(x.sjs_vm_set_clock(vm, epoch), 'sjs_vm_set_clock');
              ok(x.sjs_vm_set_seed(vm, BigInt(seed)), 'sjs_vm_set_seed');
              const run = call.struct('sjs_run_params');
              setU32(run + offset('sjs_run_params', 'program'), own);
              setU32(run + offset('sjs_run_params', 'globals'), globalValues);
              setU32(run + offset('sjs_run_params', 'global_count'), names.length);
              ok(x.sjs_vm_start(vm, run), 'sjs_vm_start');
            } finally {
              call.free();
            }
          } catch (error) {
            evaluation.state = 'cancelled';
            end();
            throw error;
          }
          return evaluation;
        };

        const compiled = {
          globals: names,
          start,
          // Evaluate to the end on at most `fuel` units, then give the result.
          evaluate(values = {}, { fuel = 10_000_000, ...options } = {}) {
            const evaluation = start(values, options);
            if (evaluation.run(fuel) === 'done') return evaluation.result;
            evaluation.cancel();
            throw new StaticJSError({ kind: 'fuel', message: `the evaluation used its ${fuel} units` });
          },
          // Give the program back; evaluations still running keep it until they end.
          release() {
            if (program === 0) return;
            x.sjs_program_release(program);
            program = 0;
            live.delete(compiled);
          },
        };
        live.add(compiled);
        return compiled;
      };

      return {
        compile,
        // Compile `source` with the names of `globals` bound, evaluate it on
        // their values, and give the program back.
        evaluate(source, { module = false, globals = {}, ...options } = {}) {
          const program = compile(source, { module, globals: Object.keys(globals) });
          try {
            return program.evaluate(globals, options);
          } finally {
            program.release();
          }
        },
        // Bytes the context's values, programs and evaluations hold now.
        get liveBytes() {
          return context === 0 ? 0 : x.sjs_context_live_bytes(context);
        },
        // End every evaluation, release every program, and give back the
        // context. Memory the module took stays with the module: wasm
        // cannot shrink it.
        close() {
          if (context === 0) return;
          for (const thing of [...live]) (thing.cancel ?? thing.release)();
          x.sjs_context_destroy(context);
          context = 0;
        },
      };
    },
  };
}
