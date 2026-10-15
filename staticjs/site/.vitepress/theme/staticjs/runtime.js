// The site's runtime: staticjs, through public/staticjs-site.wasm and its C
// layer (site/wasm/site.c), in the browser and, at build time, in Node.
// Every call takes and returns numbers; text crosses as UTF-8 in the
// module's memory. The page's live samples and the pipeline explorer use it.
import C from './constants.json';

const EVENT_WORDS = 12;
const NODE_WORDS = 9;
/** The call that reached each value of a result, by the record's `via`. */
const vias = ['sjs_vm_take_result', 'sjs_array_at', 'sjs_object_at', 'sjs_set_at', 'sjs_map_at', 'sjs_map_at'];
const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** The name of each constant group's values, by value: "SJS_EV_NUMBER" → 1 gives eventNames[1] = "NUMBER". */
function names(prefix) {
  const out = [];
  for (const [name, value] of Object.entries(C)) if (name.startsWith(prefix)) out[value] = name.slice(prefix.length);
  return out;
}
export const eventNames = names('SJS_EV_');
const opNames = names('SJS_OP_');
const opSymbols = { ADD: '+', SUBTRACT: '-', MULTIPLY: '*', DIVIDE: '/', REMAINDER: '%', LESS: '<', LESS_EQUAL: '<=',
  GREATER: '>', GREATER_EQUAL: '>=', STRICT_EQUAL: '===', STRICT_NOT_EQUAL: '!==', IN: 'in', AND: '&&', OR: '||',
  COALESCE: '??', NOT: '!', NEGATE: '-', PLUS: '+', TYPEOF: 'typeof', EXPONENT: '**' };
/** An operator's source text, from an event's count: 1 gives "+". */
export const operator = (count) => opSymbols[opNames[count]] ?? String(count);
const errorKinds = ['none', 'syntax', 'unsupported', 'name', 'limit', 'memory', 'fault'];
const stages = ['', 'parser', 'compiler', 'evaluator', 'writer'];
const valueKinds = ['undefined', 'null', 'boolean', 'number', 'string', 'bigint', 'array', 'object', 'set', 'map',
  'iterator', 'function', 'regexp', 'temporal'];

/** A staticjs instance: one module, one session (a context and an evaluator). */
export class StaticJS {
  /** @param {WebAssembly.Module} module */
  constructor(module) {
    this.x = new WebAssembly.Instance(module, {}).exports;
    this.s = this.x.sjs_site_new();
    if (!this.s) throw new Error('staticjs: no session');
  }

  static async load(url) {
    const response = await fetch(url);
    const module = WebAssembly.compileStreaming
      ? await WebAssembly.compileStreaming(response).catch(async () => WebAssembly.compile(await (await fetch(url)).arrayBuffer()))
      : await WebAssembly.compile(await response.arrayBuffer());
    return new StaticJS(module);
  }

  u8() { return new Uint8Array(this.x.memory.buffer); }
  u32(ptr, n) { return new Uint32Array(this.x.memory.buffer, ptr, n); }
  text(ptr, length) { return length === 0 ? '' : decoder.decode(this.u8().subarray(ptr, ptr + length)); }

  /** The error of the last compile or run, or null. */
  error() {
    const x = this.x, s = this.s;
    const code = x.sjs_site_error_code(s);
    if (code === 0) return null;
    return {
      kind: errorKinds[code] ?? 'error',
      stage: stages[x.sjs_site_error_stage(s)],
      line: x.sjs_site_error_line(s),
      column: x.sjs_site_error_column(s),
      offset: x.sjs_site_error_offset(s),
      message: this.text(x.sjs_site_error_message(s), x.sjs_site_error_message_length(s)),
      subject: this.text(x.sjs_site_error_subject(s), x.sjs_site_error_subject_length(s)),
    };
  }

  /**
   * Compile `source`: an expression, or a module with `module`. `chunk` bytes
   * at a time are pushed to the parser (0: whole); `shapes: false` compiles
   * without the shape matcher.
   */
  compile(source, { module = false, shapes = true, chunk = 0 } = {}) {
    const x = this.x, s = this.s;
    const bytes = encoder.encode(source);
    const ptr = x.sjs_site_source(s, bytes.length);
    this.u8().set(bytes, ptr);
    this.bytes = bytes;
    const result = x.sjs_site_compile(s, (module ? 1 : 0) | (shapes ? 0 : 2), chunk);
    return { ok: result === C.SJS_OK, error: this.error(), programBytes: x.sjs_site_program_bytes(s), chunks: x.sjs_site_chunks(s) };
  }

  /** The parser's events of the last compile, as the recording sink saw them. */
  events() {
    const x = this.x, s = this.s;
    const count = x.sjs_site_event_count(s);
    if (count === 0) return [];
    const words = this.u32(x.sjs_site_events(s), count * EVENT_WORDS);
    const numbers = new Float64Array(this.x.memory.buffer, x.sjs_site_numbers(s), count);
    const textBase = x.sjs_site_text(s);
    const out = [];
    for (let i = 0; i < count; i++) {
      const w = words.subarray(i * EVENT_WORDS, (i + 1) * EVENT_WORDS);
      const kind = eventNames[w[0]] ?? String(w[0]);
      out.push({
        kind, count: w[1], node: w[2],
        at: { offset: w[3], line: w[4], column: w[5] },
        start: { offset: w[6], line: w[7], column: w[8] },
        text: this.text(textBase + w[9], w[10]),
        chunk: w[11],
        number: kind === 'NUMBER' ? numbers[i] : undefined,
      });
    }
    return out;
  }

  /** The functions of the last program compiled: where each begins, and its shape. */
  functions() {
    const x = this.x, s = this.s;
    const out = [];
    for (let i = 0; i < x.sjs_site_function_count(s); i++) {
      const w = this.u32(x.sjs_site_function(s, i), 6);
      out.push({ at: { offset: w[0], line: w[1], column: w[2] }, params: w[3], shape: this.text(w[4], w[5]) });
    }
    return out;
  }

  /** Start an evaluation of the last program, with a clock and a seed. */
  start({ now = Date.UTC(2024, 0, 31, 10), seed = 1 } = {}) {
    this.x.sjs_site_set_clock(this.s, now);
    this.x.sjs_site_set_seed(this.s, seed);
    return this.x.sjs_site_start(this.s) === C.SJS_OK;
  }

  /** Run on `fuel` units: 'done', 'paused' or 'fault'. */
  run(fuel) {
    const status = this.x.sjs_site_run(this.s, fuel);
    return status === C.SJS_RUN_DONE ? 'done' : status === C.SJS_RUN_PAUSED ? 'paused' : 'fault';
  }

  fuelUsed() { return this.x.sjs_site_fuel_used(this.s); }
  cancel() { this.x.sjs_site_cancel(this.s); }
  resultKind() { return valueKinds[this.x.sjs_site_result_kind(this.s)] ?? null; }

  /** The evaluation's frames, outermost first. */
  frames() {
    const x = this.x, s = this.s;
    const out = [];
    for (let i = 0; i < x.sjs_site_frame_count(s); i++) {
      const w = this.u32(x.sjs_site_frame(s, i), 10);
      out.push({
        kind: w[0] === C.SJS_FRAME_LOOP ? 'loop' : 'function',
        at: { offset: w[1], line: w[2], column: w[3] },
        index: w[4], length: w[5],
        object: this.text(w[6], w[7]), name: this.text(w[8], w[9]),
      });
    }
    return out;
  }

  /**
   * The result as a host reads it through the value API, depth first, at
   * most `max` values: each one's kind, depth, element or property count,
   * the call that reached it (`via`, with its index, or a property's key, or
   * which half of a map entry), and its text.
   */
  read(max = 200) {
    const x = this.x, s = this.s;
    if (x.sjs_site_read(s, max) !== C.SJS_OK) return null;
    const base = x.sjs_site_node_text(s);
    const n = x.sjs_site_node_count(s);
    const w = this.u32(x.sjs_site_nodes(s), n * NODE_WORDS);
    const nodes = [];
    for (let i = 0; i < n; i++) {
      const r = w.subarray(i * NODE_WORDS, (i + 1) * NODE_WORDS);
      nodes.push({
        kind: valueKinds[r[0]], depth: r[1], count: r[2], via: vias[r[3]], index: r[4],
        entry: r[3] === 4 ? 'key' : r[3] === 5 ? 'value' : null,
        key: r[3] === 2 ? this.text(base + r[5], r[6]) : null,
        text: this.text(base + r[7], r[8]),
      });
    }
    return { nodes, truncated: x.sjs_site_nodes_truncated(s) === 1 };
  }

  /** Write the result as JavaScript source, through a buffer of `piece` bytes. */
  write(piece = 0) {
    const x = this.x, s = this.s;
    const r = x.sjs_site_write(s, piece);
    return { ok: r === C.SJS_OK, text: this.text(x.sjs_site_output(s), x.sjs_site_output_length(s)), pieces: x.sjs_site_pieces(s) };
  }

  /**
   * Every stage of a program, for the pipeline explorer: the compile with
   * its events and functions, the fuel with and without shapes, a trace of
   * the evaluation one unit of fuel at a time (its frames at each step), and
   * the value read through the value API and written through a small buffer.
   */
  pipeline(source, { module = false, chunk = 0, maxSteps = 4000, piece = 16 } = {}) {
    const out = { source, module, chunk };
    const plain = this.compile(source, { module, shapes: false });
    if (plain.ok) {
      this.start();
      const status = this.run(10_000_000);
      out.fuelWithoutShapes = status === 'done' ? this.fuelUsed() : null;
      this.cancel();
    }
    out.compile = this.compile(source, { module, chunk });
    out.events = this.events();
    out.bytes = this.bytes.length;
    if (!out.compile.ok) return out;
    out.functions = this.functions();
    this.start();
    const steps = [{ fuel: 0, frames: this.frames(), status: 'paused' }];
    let status = 'paused';
    while (status === 'paused' && steps.length <= maxSteps) {
      status = this.run(1);
      steps.push({ fuel: this.fuelUsed(), frames: this.frames(), status });
    }
    if (status === 'paused') status = this.run(10_000_000);
    out.steps = steps;
    out.truncated = steps.length > maxSteps;
    out.status = status;
    out.fuel = this.fuelUsed();
    if (status === 'fault') {
      out.error = this.error();
      return out;
    }
    out.kind = this.resultKind();
    out.read = this.read();
    out.output = this.write(piece);
    out.pieceSize = piece;
    return out;
  }

  /** Compile and run a whole program: what a live sample shows. */
  evaluate(source, { module = false, fuel = 10_000_000 } = {}) {
    const compiled = this.compile(source, { module });
    if (!compiled.ok) return { ok: false, error: compiled.error };
    this.start();
    const status = this.run(fuel);
    const fuelUsed = this.fuelUsed();
    if (status === 'paused') {
      this.cancel();
      return { ok: false, error: { kind: 'fuel', line: 0, column: 0, message: `ran out of its ${fuel} units of fuel`, subject: '' }, fuel: fuelUsed };
    }
    if (status === 'fault') return { ok: false, error: this.error(), fuel: fuelUsed };
    const written = this.write();
    return { ok: true, value: written.text, kind: this.resultKind(), fuel: fuelUsed };
  }
}

/** One line for a result or an error, as the page and the markdown show it. */
export function describeError(error) {
  const where = error.line ? ` at ${error.line}:${error.column}` : '';
  const label = { syntax: 'Syntax error', unsupported: 'Unsupported', name: 'Name error', limit: 'Limit reached',
    memory: 'Out of memory', fault: 'Fault', fuel: 'Out of fuel' }[error.kind] ?? 'Error';
  return `${label}${where}: ${error.message}${error.subject ? ` (${error.subject})` : ''}`;
}
