// @tinyactors/staticjs: staticjs, a small embeddable JavaScript engine for
// a pure, total subset of the language, in Node and in browsers.
//
// Everything here calls the package's own C layer (src/js.c) through the
// module's exports, which take and return numbers: values cross as handles,
// element by element, and nothing here knows a struct of staticjs's
// (docs/api-map.md says which C call each method makes).

const KINDS = ['undefined', 'null', 'boolean', 'number', 'string', 'bigint', 'array', 'object', 'set', 'map',
  'iterator', 'function', 'regexp', 'temporal'];
const ERROR_KINDS = ['', 'syntax', 'unsupported', 'name', 'limit', 'memory', 'fault'];
const NONE = 0xFFFFFFFF;
const LIMIT_NAMES = ['maxSourceBytes', 'maxTokenBytes', 'maxNesting', 'maxLookahead', 'maxHeldEvents', 'maxStack',
  'maxFrames', 'maxValueItems', 'maxStringBytes', 'maxBigintBits', 'maxRegexStates'];
// SJS_LIMITS_DEFAULT in staticjs.h, by the names above.
export const DEFAULT_LIMITS = Object.freeze({
  maxSourceBytes: 1024 * 1024,
  maxTokenBytes: 64 * 1024,
  maxNesting: 256,
  maxLookahead: 256,
  maxHeldEvents: 64 * 1024,
  maxStack: 4096,
  maxFrames: 256,
  maxValueItems: 1024 * 1024,
  maxStringBytes: 16 * 1024 * 1024,
  maxBigintBits: 256 * 1024,
  maxRegexStates: 1024 * 1024,
});
// sjs_time_type: the Temporal types staticjs has, and Date.
const TIME = { PlainDate: 1, PlainTime: 2, PlainDateTime: 3, Instant: 4, Duration: 5, Date: 6 };
const TIME_NAMES = ['', 'PlainDate', 'PlainTime', 'PlainDateTime', 'Instant', 'Duration', 'Date'];
const DURATION_FIELDS = ['years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds', 'milliseconds',
  'microseconds', 'nanoseconds'];
const PLAIN_FIELDS = ['year', 'month', 'day', 'hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond'];

// The intrinsics a conversion uses, taken once, so a patched prototype is
// never called.
const { getPrototypeOf, keys, getOwnPropertySymbols, defineProperty, freeze } = Object;
const { isArray } = Array;
const ObjectPrototype = Object.prototype;
const MapCtor = Map;
const SetCtor = Set;
const DateCtor = Date;
const RegExpCtor = RegExp;
const mapEntries = Map.prototype.entries;
const setValues = Set.prototype.values;
const mapSet = Map.prototype.set;
const setAdd = Set.prototype.add;
const dateGetTime = Date.prototype.getTime;
const arrayValues = Array.prototype.values;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

// A program refused when compiled, an evaluation that faulted, or bytes
// that are not a value. `kind` is 'syntax', 'unsupported', 'name', 'limit',
// 'memory' or 'fault'; line and column count from 1, the column in bytes.
export class StaticJSError extends Error {
  constructor({ kind, line = 0, column = 0, offset = 0, reason, subject = '', frames = [] }, options) {
    const where = line > 0 ? ` at ${line}:${column}` : '';
    super(`${kind}${where}: ${reason}${subject ? ` (${subject})` : ''}`, options);
    this.name = 'StaticJSError';
    this.kind = kind;
    this.line = line;
    this.column = column;
    this.offset = offset;
    this.reason = reason;
    this.subject = subject;
    this.frames = frames;
  }
}

// The module the package ships, beside this file.
const BUNDLED = new URL('./staticjs.wasm', import.meta.url);

async function moduleOf(source) {
  if (source instanceof WebAssembly.Module) return source;
  if (source === undefined || source instanceof URL || typeof source === 'string') {
    const url = source === undefined ? BUNDLED : new URL(source, import.meta.url);
    if (url.protocol === 'file:') {
      const { readFile } = await import('node:fs/promises');
      return WebAssembly.compile(await readFile(url));
    }
    source = fetch(url);
  }
  if (source instanceof Promise) source = await source;
  if (typeof Response !== 'undefined' && source instanceof Response) {
    if (WebAssembly.compileStreaming) {
      try {
        return await WebAssembly.compileStreaming(source.clone());
      } catch {
        // A server that does not give application/wasm: compile the bytes.
      }
    }
    return WebAssembly.compile(await source.arrayBuffer());
  }
  return WebAssembly.compile(source);
}

// Load the module: the one this package ships, or `source` (a URL, a path
// relative to this file, a Response, bytes or a WebAssembly.Module). Each
// load is an instance of its own: contexts in it share its memory.
export async function load(source) {
  const module = await moduleOf(source);
  return new StaticJS(module);
}

class StaticJS {
  #x;
  #natives = [];          // slot -> { fn, generation }, a native's JavaScript function
  #freeNatives = [];
  #pendingCause = null;   // what a native threw, for the fault it causes
  #running = null;        // the context running now: a step, a conversion, a native

  constructor(module) {
    const imports = {
      staticjs_js: {
        native: (self, base, count) => this.#callNative(self, base, count),
        release: (self) => this.#releaseNative(self),
      },
    };
    this.#x = new WebAssembly.Instance(module, imports).exports;
    const v = this.#x.sjs_version();
    this.version = `${v >> 16}.${(v >> 8) & 255}.${v & 255}`;
  }

  // A context: its own limits, and the values, programs and runs made in
  // it. `maxItems` bounds each conversion between JavaScript and staticjs
  // (default: limits.maxValueItems), as fuel bounds an evaluation and the
  // limits a value: a small result can stand for a tree far larger.
  createContext({ limits = {}, maxItems } = {}) {
    return new Context(this, this.#x, { limits, maxItems });
  }

  // --- for the classes below ---------------------------------------------
  get _running() { return this.#running; }
  set _running(context) { this.#running = context; }

  _addNative(fn) {
    let slot = this.#freeNatives.pop();
    if (slot === undefined) slot = this.#natives.length;
    const previous = this.#natives[slot];
    const generation = ((previous?.generation ?? 0) + 1) & 0x7FFF;
    this.#natives[slot] = { fn, generation, context: null };
    return (slot | (generation << 16)) >>> 0;
  }

  _nativeContext(self, context) {
    this.#natives[self & 0xFFFF].context = context;
  }

  _takeCause() {
    const cause = this.#pendingCause;
    this.#pendingCause = null;
    return cause;
  }

  #callNative(self, base, count) {
    const entry = this.#natives[self & 0xFFFF];
    if (entry === undefined || entry.generation !== self >>> 16) return this.#nativeFault(new Error('a native function that is gone'));
    const context = entry.context;
    try {
      const args = [];
      for (let i = 0; i < count; i++) args.push(context._toJS(base + i, { borrowed: true }));
      const result = entry.fn(...args);
      return context._toHandle(result);
    } catch (error) {
      return this.#nativeFault(error);
    }
  }

  #nativeFault(error) {
    this.#pendingCause = error;
    const message = error instanceof Error ? error.message : String(error);
    const bytes = encoder.encode(message);
    const at = this.#x.sjs_js_scratch(bytes.length || 1);
    new Uint8Array(this.#x.memory.buffer, at, bytes.length).set(bytes);
    this.#x.sjs_js_native_fault(bytes.length);
    return -1;
  }

  #releaseNative(self) {
    const slot = self & 0xFFFF;
    const entry = this.#natives[slot];
    if (entry === undefined || entry.generation !== self >>> 16) return;
    this.#natives[slot] = { fn: null, generation: entry.generation, context: null };
    // A slot whose generation would wrap is retired, so a stale self never
    // names a later function.
    if (entry.generation !== 0x7FFF) this.#freeNatives.push(slot);
  }
}

// Releases what an unreachable object still held in staticjs; dispose()
// is the way, this the fallback, which may never run.
const finalizers = new FinalizationRegistry((drop) => drop());

class Context {
  #js;
  #x;
  #c;              // the C layer's context
  #maxItems;
  #maxStringBytes;
  #run = null;     // the run that holds the evaluator
  #programs = new Map();   // C pointer -> Program, for frames

  constructor(js, x, { limits, maxItems }) {
    this.#js = js;
    this.#x = x;
    const all = { ...DEFAULT_LIMITS };
    for (const [name, value] of Object.entries(limits)) {
      if (!(name in DEFAULT_LIMITS)) throw new TypeError(`no limit is called ${name}`);
      if (!Number.isInteger(value) || value < 0 || value > 0xFFFFFFFF) throw new RangeError(`${name} is a 32-bit count`);
      all[name] = value;
    }
    const words = new Uint32Array(LIMIT_NAMES.map((name) => all[name]));
    new Uint8Array(x.memory.buffer, x.sjs_js_scratch(words.byteLength), words.byteLength).set(new Uint8Array(words.buffer));
    this.#c = x.sjs_js_context_new();
    if (this.#c === 0) throw this.#error();
    this.#maxItems = maxItems ?? all.maxValueItems;
    this.#maxStringBytes = all.maxStringBytes;
  }

  // --- the C layer --------------------------------------------------------

  #check() {
    if (this.#c === 0) throw new TypeError('the context is disposed');
    if (this.#js._running !== null) throw new TypeError('the context is running');
  }

  // The error the last failing call left, as an exception.
  #error(frames = []) {
    const x = this.#x;
    const w = new Uint32Array(x.memory.buffer, x.sjs_js_error(), 8).slice();
    const text = (ptr, length) => decoder.decode(new Uint8Array(x.memory.buffer, ptr, length));
    const reason = text(w[4], w[5]);
    if (w[0] & 0x80000000) {
      const result = -(w[0] & 0x7FFFFFFF);
      if (result === -3) return new StaticJSError({ kind: 'limit', reason });
      if (result === -2) return new StaticJSError({ kind: 'memory', reason });
      return new TypeError(reason);
    }
    return new StaticJSError({ kind: ERROR_KINDS[w[0]], offset: w[1], line: w[2], column: w[3], reason,
      subject: text(w[6], w[7]), frames });
  }

  #scratch(bytes) {
    const at = this.#x.sjs_js_scratch(bytes.length || 1);
    new Uint8Array(this.#x.memory.buffer, at, bytes.length).set(bytes);
    return bytes.length;
  }

  #text(ptrFn = 'sjs_js_text_ptr', lenFn = 'sjs_js_text_len') {
    const x = this.#x;
    return decoder.decode(new Uint8Array(x.memory.buffer, x[ptrFn](), x[lenFn]()));
  }

  #handle(h) {
    if (h === NONE || h === -1) throw this.#error();
    return h;
  }

  // Run `work` with the context running and the stack released to where it
  // was, whatever happens.
  #within(work) {
    this.#check();
    const x = this.#x;
    const mark = x.sjs_js_mark();
    this.#js._running = this;
    try {
      return work(mark);
    } finally {
      this.#js._running = null;
      x.sjs_js_release_to(this.#c, mark);
    }
  }

  // --- conversions --------------------------------------------------------

  // A JavaScript value onto the stack: its handle. Iterative, so depth costs
  // memory, not the call stack; counted, so a value with shared parts that
  // unfolds into a large tree stops at maxItems.
  _toHandle(input) {
    const x = this.#x;
    const c = this.#c;
    let items = 0;
    const count = () => {
      if (++items > this.#maxItems) throw new RangeError(`the value is more than ${this.#maxItems} items (maxItems)`);
    };
    // Each frame: a container's handle and what goes into it.
    const frames = [];
    let result;
    // A value: a scalar's handle, or a container's, with a frame to fill.
    const begin = (v) => {
      count();
      switch (typeof v) {
        case 'undefined': return x.sjs_js_undefined();
        case 'boolean': return x.sjs_js_boolean(v ? 1 : 0);
        case 'number': return x.sjs_js_number(v);
        case 'string':
          if (!v.isWellFormed()) throw new TypeError('a string with a lone surrogate: a staticjs string cannot hold one');
          return this.#handle(x.sjs_js_string(c, this.#scratch(encoder.encode(v))));
        case 'bigint': return this.#handle(x.sjs_js_bigint(c, this.#scratch(encoder.encode(v.toString()))));
        case 'function': return this.#handle(this.#native(v));
        case 'object': break;
        default: throw new TypeError(`staticjs has no value for a ${typeof v}`);
      }
      if (v === null) return x.sjs_js_null();
      if (v instanceof Value) return v._push(this);
      const time = timeFields(v);
      if (time !== null) {
        const words = new Float64Array(time);
        new Uint8Array(x.memory.buffer, x.sjs_js_scratch(words.byteLength), words.byteLength).set(new Uint8Array(words.buffer));
        return this.#handle(x.sjs_js_time(c));
      }
      if (isArray(v)) {
        const h = this.#handle(x.sjs_js_container(c, 0, v.length));
        for (let i = 0; i < v.length; i++) if (!(i in v)) throw new TypeError('an array with a hole: staticjs arrays have none');
        frames.push({ h, kind: 0, items: v, i: 0 });
        return h;
      }
      if (v instanceof MapCtor) {
        const h = this.#handle(x.sjs_js_container(c, 3, v.size));
        frames.push({ h, kind: 3, items: [...mapEntries.call(v)], i: 0, half: 0 });
        return h;
      }
      if (v instanceof SetCtor) {
        const h = this.#handle(x.sjs_js_container(c, 2, v.size));
        frames.push({ h, kind: 2, items: [...setValues.call(v)], i: 0 });
        return h;
      }
      const prototype = getPrototypeOf(v);
      if (prototype === ObjectPrototype || prototype === null) {
        if (getOwnPropertySymbols(v).length > 0) throw new TypeError('an object with a symbol key: staticjs has none');
        const names = keys(v);
        const h = this.#handle(x.sjs_js_container(c, 1, names.length));
        frames.push({ h, kind: 1, items: names, i: 0, object: v });
        return h;
      }
      if (v instanceof RegExpCtor) throw new TypeError('a RegExp: a program writes its own literal');
      throw new TypeError(`staticjs has no value for a ${v.constructor?.name ?? 'object'}`);
    };
    // Put the value at the top into the frame's container, and drop the
    // slots its making left above the container.
    const put = (frame, key) => {
      let r;
      if (frame.kind === 1) r = x.sjs_js_object_put(c, frame.h, this.#scratch(encoder.encode(key)));
      else if (frame.kind === 3) r = x.sjs_js_map_set(c, frame.h);
      else r = x.sjs_js_append(c, frame.h, x.sjs_js_mark() - 1);
      if (r !== 0) throw this.#error();
      x.sjs_js_release_to(c, frame.h + 1);
    };
    result = begin(input);
    while (frames.length > 0) {
      const frame = frames[frames.length - 1];
      if (frame.done) {
        frames.pop();
        if (frames.length > 0) {
          const parent = frames[frames.length - 1];
          this.#afterChild(parent, put);
        }
        continue;
      }
      if (frame.i === frame.items.length) {
        frame.done = true;
        continue;
      }
      let child;
      if (frame.kind === 1) {
        const key = frame.items[frame.i];
        if (!key.isWellFormed()) throw new TypeError('a key with a lone surrogate: a staticjs string cannot hold one');
        frame.key = key;
        child = frame.object[key];
      } else if (frame.kind === 3) {
        const [key, value] = frame.items[frame.i];
        if (frame.half === 0) {
          if (key !== null && (typeof key === 'object' || typeof key === 'function')) {
            throw new TypeError('a Map\'s key in staticjs is a primitive');
          }
          frame.half = 1;
          begin(key);
          continue;
        }
        child = value;
      } else {
        child = frame.items[frame.i];
      }
      const depth = frames.length;
      begin(child);
      // A scalar is done at once; a container is done when its frame is.
      if (frames.length === depth) this.#afterChild(frame, put);
    }
    return result;
  }

  // The child at the top went into `frame`: put it, and move on.
  #afterChild(frame, put) {
    if (frame.kind === 3) {
      put(frame);
      frame.half = 0;
    } else {
      put(frame, frame.key);
    }
    frame.i++;
  }

  #native(fn) {
    const self = this.#js._addNative(fn);
    this.#js._nativeContext(self, this);
    return this.#x.sjs_js_native(this.#c, self);
  }

  // The value at handle `h` as JavaScript. A function stays in staticjs, as
  // a Value; `borrowed` is for a native's arguments. Iterative and counted.
  _toJS(root, { json = false } = {}) {
    const x = this.#x;
    const c = this.#c;
    let items = 0;
    const keyText = new Map();   // a key's pointer -> its text, for this conversion only
    const frames = [];
    let result;
    const memory = () => x.memory.buffer;
    // A scalar's value, or a container with a frame to fill.
    const begin = (h) => {
      if (++items > this.#maxItems) throw new RangeError(`the value is more than ${this.#maxItems} items (maxItems)`);
      const kind = KINDS[x.sjs_js_kind(h)];
      switch (kind) {
        case 'undefined': return undefined;
        case 'null': return null;
        case 'boolean': return x.sjs_js_boolean_of(h) !== 0;
        case 'number': return x.sjs_js_number_of(h);
        case 'string':
          x.sjs_js_string_of(h);
          return this.#text();
        case 'bigint':
          if (x.sjs_js_bigint_of(c, h) !== 0) throw this.#error();
          return BigInt(this.#text());
        case 'regexp':
          x.sjs_js_regexp_of(h);
          return new RegExpCtor(this.#text(), this.#text('sjs_js_text2_ptr', 'sjs_js_text2_len'));
        case 'temporal': return timeValue(new Float64Array(memory(), x.sjs_js_time_of(h), 11));
        case 'function': return new Value(this, x.sjs_js_keep(c, h));
        default: break;
      }
      const length = x.sjs_js_length(h);
      let target;
      if (kind === 'array' || kind === 'iterator') target = [];
      else if (kind === 'set') target = new SetCtor();
      else if (kind === 'map') target = new MapCtor();
      else target = {};
      frames.push({ h, kind, i: 0, length, target, mark: x.sjs_js_mark() });
      return target;
    };
    const add = (frame, value, key) => {
      if (frame.kind === 'array' || frame.kind === 'iterator') frame.target.push(value);
      else if (frame.kind === 'set') setAdd.call(frame.target, value);
      else if (frame.kind === 'map') mapSet.call(frame.target, key, value);
      else defineProperty(frame.target, key, { value, enumerable: true, writable: true, configurable: true });
    };
    result = begin(root);
    while (frames.length > 0) {
      const frame = frames[frames.length - 1];
      x.sjs_js_release_to(c, frame.mark);
      if (frame.i === frame.length) {
        frames.pop();
        if (frame.kind === 'iterator') frame.done = arrayValues.call(frame.target);
        if (frames.length > 0 && frame.kind === 'iterator') {
          // An iterator went into its parent as an array; it is an
          // iterator over those values.
          const parent = frames[frames.length - 1];
          replaceLast(parent, frame.target, frame.done);
        } else if (frame.kind === 'iterator') {
          result = frame.done;
        }
        continue;
      }
      const i = frame.i++;
      if (frame.kind === 'object') {
        const valueHandle = x.sjs_js_property(frame.h, i);
        const ptr = x.sjs_js_text_ptr();
        let key = keyText.get(ptr);
        if (key === undefined) {
          key = this.#text();
          keyText.set(ptr, key);
        }
        const depth = frames.length;
        const value = begin(valueHandle);
        add(frame, value, key);
        if (frames.length !== depth) frames[frames.length - 1].parentKey = key;
      } else if (frame.kind === 'map') {
        const keyHandle = x.sjs_js_entry(frame.h, i);
        const key = begin(keyHandle);   // a primitive
        const depth = frames.length;
        const value = begin(keyHandle + 1);
        add(frame, value, key);
        if (frames.length !== depth) frames[frames.length - 1].parentKey = key;
      } else {
        const item = x.sjs_js_item(frame.h, i);
        if (item === NONE) throw this.#error();
        if (item & 0x80000000) {
          // An entries iterator's item: [key, value].
          const keyHandle = (item & 0x7FFFFFFF) >>> 0;
          const pair = [begin(keyHandle)];
          const depth = frames.length;
          pair.push(begin(keyHandle + 1));
          add(frame, pair);
          if (frames.length !== depth) frames[frames.length - 1].parentKey = 1;
        } else {
          add(frame, begin(item));
        }
      }
    }
    void json;
    return result;
  }

  // --- the API --------------------------------------------------------------

  // A program: `source` compiled as an expression, or as a module (its
  // value is the object of its exports). `globals` names the values each
  // run binds; `modules` maps a name a program imports to a Value, a
  // module's exports (a run's resultValue()).
  compile(source, { module = false, globals = [], modules = {} } = {}) {
    return this.#within(() => {
      const x = this.#x;
      x.sjs_js_compile_begin(this.#c);
      for (const [name, exports] of Object.entries(modules)) {
        if (!(exports instanceof Value)) throw new TypeError(`the module ${name} is a Value: a module's exports`);
        const h = exports._push(this);
        if (x.sjs_js_compile_module(this.#scratch(encoder.encode(name)), h) !== 0) throw new StaticJSError({ kind: 'memory', reason: 'out of memory' });
      }
      for (const name of globals) {
        if (typeof name !== 'string') throw new TypeError('a global is named by a string');
        if (x.sjs_js_compile_global(this.#scratch(encoder.encode(name))) !== 0) throw this.#error();
      }
      const p = x.sjs_js_compile(this.#c, module ? 1 : 0, this.#scratch(encoder.encode(source)));
      if (p === 0) throw this.#error();
      const program = new Program(this, p, freeze([...globals]));
      this.#programs.set(p, program);
      return program;
    });
  }

  // A JavaScript value made once in staticjs, to give to many runs.
  value(input) {
    return this.#within(() => new Value(this, this.#x.sjs_js_keep(this.#c, this._toHandle(input))));
  }

  // A value read from JSON (a string, or UTF-8 bytes) as JSON.parse reads it.
  fromJSON(json) {
    const bytes = typeof json === 'string' ? encoder.encode(json) : json;
    return this.#read(1, bytes);
  }

  // A value read from one CBOR neutral value (tinyactors' protocol): an
  // integer within +-(2^53 - 1) as a number and any other as a BigInt.
  fromCBOR(bytes) {
    return this.#read(2, bytes);
  }

  #read(format, bytes) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError('bytes are a Uint8Array');
    return this.#within(() => {
      const h = this.#handle(this.#x.sjs_js_read(this.#c, format, this.#scratch(bytes)));
      return new Value(this, this.#x.sjs_js_keep(this.#c, h));
    });
  }

  // The clock a program reads (Temporal.Now, Date.now(), new Date()):
  // anything with epochMilliseconds or epochNanoseconds, such as a
  // Temporal.Instant. A program that reads the clock before it is set
  // faults.
  setClock(instant) {
    this.#check();
    let ns;
    if (typeof instant?.epochNanoseconds === 'bigint') ns = instant.epochNanoseconds;
    else if (Number.isFinite(instant?.epochMilliseconds)) ns = BigInt(Math.floor(instant.epochMilliseconds)) * 1000000n;
    else throw new TypeError('setClock takes { epochMilliseconds } or { epochNanoseconds }');
    let seconds = ns / 1000000000n;
    let rest = ns % 1000000000n;
    if (rest < 0n) {
      seconds -= 1n;
      rest += 1000000000n;
    }
    if (this.#x.sjs_js_set_clock(this.#c, Number(seconds), Number(rest)) !== 0) {
      throw new RangeError('the instant is past Temporal\'s limits');
    }
  }

  // Move a set clock forward by { milliseconds } or { nanoseconds }.
  advanceClock({ milliseconds = 0, nanoseconds = 0 } = {}) {
    this.#check();
    const ns = milliseconds * 1e6 + Number(nanoseconds);
    if (!Number.isSafeInteger(ns) || ns < 0) throw new RangeError('the clock moves forward, by whole nanoseconds');
    if (this.#x.sjs_js_advance_clock(this.#c, ns) !== 0) throw new RangeError('the clock is not set, or would pass Temporal\'s limits');
  }

  // What Math.random draws from, as `node --random-seed=<seed>` does.
  setSeed(seed) {
    this.#check();
    if (!Number.isSafeInteger(seed)) throw new RangeError('a seed is an integer');
    this.#x.sjs_js_set_seed(this.#c, seed);
  }

  // Bytes the context's values, programs and runs hold now.
  get liveBytes() {
    return this.#c === 0 ? 0 : this.#x.sjs_js_live_bytes(this.#c);
  }

  // End the run, release every program and value, and free the context.
  dispose() {
    if (this.#c === 0) return;
    this.#check();
    this.#run?._end('the context is disposed');
    this.#x.sjs_js_cancel(this.#c);
    this.#x.sjs_js_drop_rows(this.#c);
    for (const program of this.#programs.values()) program._end();
    this.#programs.clear();
    this.#x.sjs_js_context_free(this.#c);
    this.#c = 0;
  }

  [Symbol.dispose]() { this.dispose(); }

  // --- for Program, Run and Value ------------------------------------------

  get _x() { return this.#x; }
  get _c() { return this.#c; }
  _within(work) { return this.#within(work); }
  _error(frames) { return this.#error(frames); }
  _forget(p) { this.#programs.delete(p); }

  // A run takes the evaluator: a paused one is abandoned.
  _begin(run) {
    this.#run?._end('a later start on the context abandoned this run');
    this.#run = run;
  }

  _release(run) {
    if (this.#run === run) this.#run = null;
  }

  _frames() {
    const x = this.#x;
    const frames = [];
    for (let i = 0, n = x.sjs_js_frame_count(this.#c); i < n; i++) {
      const w = new Uint32Array(x.memory.buffer, x.sjs_js_frame(this.#c, i), 8).slice();
      const object = this.#text();
      const name = this.#text('sjs_js_text2_ptr', 'sjs_js_text2_len');
      const frame = { kind: w[0] === 2 ? 'loop' : 'function', offset: w[1], line: w[2], column: w[3] };
      if (w[0] === 2) Object.assign(frame, { object, name, index: w[4], length: w[5] });
      else Object.assign(frame, { program: this.#programs.get(w[7]), function: w[6] });
      frames.push(freeze(frame));
    }
    return frames;
  }

  _causeOfFault() { return this.#js._takeCause(); }
}

// An iterator's values became an array in its parent: put the iterator
// there instead.
function replaceLast(parent, array, iterator) {
  const t = parent.target;
  if (isArray(t)) {
    const i = t.lastIndexOf(array);
    if (i >= 0) t[i] = iterator;
  } else if (t instanceof SetCtor || t instanceof MapCtor) {
    // A set's member and a map's key are primitives; a map's value is the
    // one that was the iterator.
    if (t instanceof MapCtor) for (const [k, v] of t) if (v === array) mapSet.call(t, k, iterator);
  } else {
    for (const k of keys(t)) if (t[k] === array) t[k] = iterator;
  }
}

// A Date or a Temporal value as sjs_js_time's doubles, or null.
function timeFields(v) {
  if (v instanceof DateCtor) return [TIME.Date, dateGetTime.call(v)];
  const T = globalThis.Temporal;
  if (T === undefined) return null;
  for (const name of ['PlainDate', 'PlainTime', 'PlainDateTime']) {
    if (v instanceof T[name]) {
      if (name !== 'PlainTime' && v.calendarId !== 'iso8601') throw new TypeError('staticjs has the ISO 8601 calendar only');
      const f = PLAIN_FIELDS.map((field) => (name === 'PlainTime' && ['year', 'month', 'day'].includes(field) ? 0 : v[field] ?? 0));
      return [TIME[name], ...f];
    }
  }
  if (v instanceof T.Instant) {
    const ns = v.epochNanoseconds;
    let seconds = ns / 1000000000n;
    let rest = ns % 1000000000n;
    if (rest < 0n) {
      seconds -= 1n;
      rest += 1000000000n;
    }
    return [TIME.Instant, Number(seconds), Number(rest)];
  }
  if (v instanceof T.Duration) return [TIME.Duration, ...DURATION_FIELDS.map((f) => v[f])];
  if (v instanceof T.ZonedDateTime || v instanceof T.PlainYearMonth || v instanceof T.PlainMonthDay) {
    throw new TypeError(`staticjs has no Temporal.${v.constructor.name}`);
  }
  return null;
}

// sjs_js_time_of's doubles as a JavaScript Date or Temporal value.
function timeValue(f) {
  const type = TIME_NAMES[f[0]];
  if (type === 'Date') return new DateCtor(f[1]);
  const T = globalThis.Temporal;
  if (T === undefined) throw new TypeError(`this JavaScript has no Temporal for a Temporal.${type}`);
  switch (type) {
    case 'PlainDate': return new T.PlainDate(f[1], f[2], f[3]);
    case 'PlainTime': return new T.PlainTime(f[4], f[5], f[6], f[7], f[8], f[9]);
    case 'PlainDateTime': return new T.PlainDateTime(f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
    case 'Instant': return new T.Instant(BigInt(f[1]) * 1000000000n + BigInt(f[2]));
    default: return new T.Duration(...Array.from(f.subarray(1, 11)));
  }
}

class Program {
  #context;
  #p;
  constructor(context, p, globals) {
    this.#context = context;
    this.#p = p;
    this.globals = globals;
    finalizers.register(this, () => {}, this);
  }

  #check() {
    if (this.#p === 0) throw new TypeError('the program is disposed');
  }

  // Each function of the program, its own code first: where it begins, its
  // parameters, and the shape the compiler tagged it with, or ''.
  functions() {
    this.#check();
    const x = this.#context._x;
    const out = [];
    for (let i = 0, n = x.sjs_js_function_count(this.#p); i < n; i++) {
      const w = new Uint32Array(x.memory.buffer, x.sjs_js_function(this.#p, i), 4).slice();
      const shape = decoder.decode(new Uint8Array(x.memory.buffer, x.sjs_js_text_ptr(), x.sjs_js_text_len()));
      out.push(freeze({ offset: w[0], line: w[1], column: w[2], params: w[3], shape }));
    }
    return out;
  }

  // A run of the program on the values of its globals, by name (a missing
  // one is a TypeError; others are left out). A function among them is a
  // native the program can call. The run takes the context's evaluator: a
  // paused run of the context is abandoned.
  start(globals = {}) {
    this.#check();
    const context = this.#context;
    return context._within(() => {
      const x = context._x;
      for (const name of this.globals) {
        if (!(name in globals)) throw new TypeError(`no value for the global ${name}`);
        context._toHandle(globals[name]);
      }
      const run = new Run(context);
      context._begin(run);
      if (x.sjs_js_start(context._c, this.#p, this.globals.length) !== 0) {
        context._release(run);
        throw context._error();
      }
      return run;
    });
  }

  dispose() {
    if (this.#p === 0) return;
    this.#context._forget(this.#p);
    this.#context._x.sjs_js_program_release(this.#p);
    this.#p = 0;
  }

  [Symbol.dispose]() { this.dispose(); }

  _end() {
    if (this.#p !== 0) this.#context._x.sjs_js_program_release(this.#p);
    this.#p = 0;
  }
}

class Run {
  #context;
  #state = 'paused';     // 'paused', 'done', 'faulted', 'ended'
  #why = '';
  #result = null;        // the result, kept as a Value, once done
  fuelUsed = 0;

  constructor(context) {
    this.#context = context;
  }

  get state() { return this.#state; }

  #paused() {
    if (this.#state === 'ended') throw new TypeError(this.#why);
    if (this.#state !== 'paused') throw new TypeError(`the run is ${this.#state}`);
  }

  // Run on `fuel` more units: true when the run is done, false when the
  // fuel is spent (it stays paused, and step(more) resumes it). A fault
  // throws a StaticJSError, with the frames where it faulted.
  step(fuel) {
    this.#paused();
    if (!(Number.isSafeInteger(fuel) && fuel > 0)) throw new RangeError('fuel is a positive whole number');
    const context = this.#context;
    return context._within(() => {
      const x = context._x;
      const status = x.sjs_js_step(context._c, fuel);
      this.fuelUsed = x.sjs_js_fuel_used(context._c);
      if (status === 1) return false;
      if (status === 2) {
        const error = context._error(context._frames());
        const cause = context._causeOfFault();
        if (cause !== null) error.cause = cause;
        this.#state = 'faulted';
        x.sjs_js_cancel(context._c);
        context._release(this);
        throw error;
      }
      const h = x.sjs_js_take(context._c);
      this.#result = new Value(context, x.sjs_js_keep(context._c, h));
      this.#state = 'done';
      context._release(this);
      return true;
    });
  }

  // Where a paused run is: its frames, outermost first.
  frames() {
    this.#paused();
    return this.#context._frames();
  }

  #done() {
    if (this.#state !== 'done') throw new TypeError(this.#state === 'ended' ? this.#why : `the run is ${this.#state}, not done`);
    return this.#result;
  }

  // The result as JavaScript, once done.
  result() {
    return this.#done().toJS();
  }

  // The result kept in staticjs, once done: a Value of its own.
  resultValue() {
    return this.#done()._copy();
  }

  // Abandon a paused run.
  cancel() {
    if (this.#state !== 'paused') return;
    this.#context._within(() => this.#context._x.sjs_js_cancel(this.#context._c));
    this.#context._release(this);
    this.#state = 'ended';
    this.#why = 'the run was cancelled';
  }

  dispose() {
    this.cancel();
    this.#result?.dispose();
    this.#result = null;
  }

  [Symbol.dispose]() { this.dispose(); }

  _end(why) {
    if (this.#state === 'paused') {
      this.#state = 'ended';
      this.#why = why;
    }
  }
}

class Value {
  #context;
  #row;
  #generation;

  constructor(context, row) {
    if (row === NONE) throw context._error();
    this.#context = context;
    this.#row = row;
    this.#generation = context._x.sjs_js_row_generation(row);
    const x = context._x;
    const generation = this.#generation;
    finalizers.register(this, () => x.sjs_js_drop(row, generation), this);
  }

  #check() {
    if (this.#row === NONE || this.#context._x.sjs_js_row_generation(this.#row) !== this.#generation) {
      throw new TypeError('the value is disposed');
    }
  }

  // sjs_kind: 'undefined', 'null', 'boolean', 'number', 'string', 'bigint',
  // 'array', 'object', 'set', 'map', 'iterator', 'function', 'regexp' or
  // 'temporal' (a Date or a Temporal value).
  get kind() {
    this.#check();
    const x = this.#context._x;
    const mark = x.sjs_js_mark();
    try {
      return KINDS[x.sjs_js_kind(x.sjs_js_row(this.#row))];
    } finally {
      x.sjs_js_release_to(this.#context._c, mark);
    }
  }

  // The value as JavaScript: exact (BigInt, Map, Set, undefined, -0, Date),
  // a regular expression as a RegExp, a Temporal value as the runtime's
  // Temporal (a TypeError where it has none), an iterator as an array
  // iterator over what it has left, a function as a Value.
  toJS() {
    return this.#reading((h) => this.#context._toJS(h));
  }

  // JavaScript source that evaluates to an equal value.
  toString() {
    return decoder.decode(this.#write(0));
  }

  // JSON text, as JSON.stringify gives it in staticjs.
  json() {
    return decoder.decode(this.#write(1));
  }

  // One CBOR neutral value, as tinyactors' ECMAScript data model writes it.
  cbor() {
    return this.#write(2).slice();
  }

  #write(format) {
    return this.#reading((h) => {
      const x = this.#context._x;
      const r = x.sjs_js_write(this.#context._c, h, format);
      if (r === -5) throw this.#context._error();
      if (r === -3) throw new StaticJSError({ kind: 'limit', reason: 'the text is longer than maxStringBytes' });
      if (r !== 0) throw new StaticJSError({ kind: 'memory', reason: 'out of memory' });
      return new Uint8Array(x.memory.buffer, x.sjs_js_text_ptr(), x.sjs_js_text_len());
    });
  }

  #reading(work) {
    this.#check();
    return this.#context._within(() => work(this.#context._x.sjs_js_row(this.#row)));
  }

  // A run that calls this function (one a program made) with `args`.
  start(args = []) {
    this.#check();
    const context = this.#context;
    return context._within(() => {
      const x = context._x;
      const f = x.sjs_js_row(this.#row);
      for (const arg of args) context._toHandle(arg);
      const run = new Run(context);
      context._begin(run);
      if (x.sjs_js_call(context._c, f, args.length) !== 0) {
        context._release(run);
        throw context._error();
      }
      return run;
    });
  }

  dispose() {
    if (this.#row === NONE) return;
    this.#context._x.sjs_js_drop(this.#row, this.#generation);
    finalizers.unregister(this);
    this.#row = NONE;
  }

  [Symbol.dispose]() { this.dispose(); }

  // For a conversion in: this value onto the stack, borrowed.
  _push(context) {
    this.#check();
    if (context !== this.#context) throw new TypeError('a value of another context');
    return context._x.sjs_js_row(this.#row);
  }

  _copy() {
    return this.#reading((h) => new Value(this.#context, this.#context._x.sjs_js_keep(this.#context._c, h)));
  }
}

export { Value, Program, Run, Context, StaticJS };
