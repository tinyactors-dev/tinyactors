/**
 * wasm adapter for the synchronous Node-API subset in src/node_api.h.
 * All actor/datamodel/host semantics live in ../shared/binding.c. This file
 * knows only JS values, scoped handles, strong references, callbacks and bytes.
 * instantiate(imports) may return an Instance or a Promise<Instance>.
 */
export function createNative(instantiate) {
  let w;
  const enc = new TextEncoder(), dec = new TextDecoder();
  const values = [undefined]; // 0 is a NULL napi_value, not JS undefined
  const refs = [undefined], freeRefs = [];
  const frames = [];
  const externals = new WeakMap();
  const buffers = new Map(); // temporary wasm mirrors of JS ArrayBuffers
  let pending = false, exception;
  // Views of wasm memory, made again only when it grew (its old buffer is then
  // detached: views of it have length 0). Every napi call reads or writes it.
  let u8 = new Uint8Array(0), dv = new DataView(new ArrayBuffer(0));
  const bytes = () => {
    if (!u8.length) {
      u8 = new Uint8Array(w.memory.buffer);
      dv = new DataView(w.memory.buffer);
    }
    return u8;
  };
  const mem = () => (bytes(), dv);
  const read = (p) => mem().getUint32(p, true);
  const write = (p, v) => { if (p) mem().setUint32(p, v, true); };
  const boolean = (p, v) => { if (p) mem().setUint8(p, v ? 1 : 0); };
  const put = (v) => values.push(v) - 1;
  const result = (p, v) => write(p, put(v));
  function string(p, n = -1) {
    p >>>= 0;
    const b = bytes();
    if ((n >>> 0) === 0xffffffff) {
      let end = p;
      while (b[end]) end++;
      n = end - p;
    }
    if (n <= 32) { // short ASCII (event names, keys): no TextDecoder round trip
      let s = '';
      for (let i = p; i < p + n; i++) {
        const c = b[i];
        if (c >= 0x80) return dec.decode(b.subarray(p, p + n));
        s += String.fromCharCode(c);
      }
      return s;
    }
    return dec.decode(b.subarray(p, p + n));
  }
  // Property names in *_named_property calls are C string literals (node_api.h):
  // immutable, at fixed addresses, so decoded once.
  const names = new Map();
  const name = (p) => {
    let s = names.get(p);
    if (s === undefined) names.set(p, (s = Object.keys({ [string(p)]: 0 })[0])); // (internalized: a fast key)
    return s;
  };
  function utf8Length(s) { // as TextEncoder writes it (lone surrogates: U+FFFD)
    let n = s.length;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c < 0x80) continue;
      if (c < 0x800) n += 1;
      else if (c <= 0xdbff && c >= 0xd800 && (s.charCodeAt(i + 1) & 0xfc00) === 0xdc00) (n += 2), i++;
      else n += 2;
    }
    return n;
  }
  function fail(e) { pending = true; exception = e; return 10; }

  // A native callback owns temporary handles and byte mirrors. Nested callbacks
  // (notably In()) get their own scope; references survive either scope ending.
  function scope(fn, frame = {}) { // frame: { args, self, data }; buffers added when mirrored
    const base = values.length;
    frames.push(frame);
    try {
      const value = fn();
      if (pending) {
        const e = exception;
        pending = false;
        exception = undefined;
        throw e;
      }
      return value;
    } finally {
      if (frame.buffers) {
        syncToJS();
        for (const buffer of frame.buffers) {
          w.w_free(buffers.get(buffer));
          buffers.delete(buffer);
        }
      } else if (buffers.size) syncToJS();
      frames.pop();
      values.length = base;
    }
  }
  function syncToJS() {
    for (const [buffer, p] of buffers)
      new Uint8Array(buffer).set(bytes().subarray(p, p + buffer.byteLength));
  }
  function syncToC() {
    for (const [buffer, p] of buffers) bytes().set(new Uint8Array(buffer), p);
  }
  function mirror(buffer) {
    if (!buffers.has(buffer)) {
      const p = w.w_alloc(buffer.byteLength);
      if (!p) throw new Error('out of wasm memory');
      buffers.set(buffer, p);
      const frame = frames.at(-1);
      (frame.buffers || (frame.buffers = [])).push(buffer);
      bytes().set(new Uint8Array(buffer), p);
    }
    return buffers.get(buffer);
  }
  function callback(cb, data, name) {
    return Object.defineProperty(function (...args) {
      if (buffers.size) syncToC();
      return scope(() => values[w.w_call(cb, frames.length)], { args, self: this, data });
    }, 'name', { value: name, configurable: true });
  }
  const finalizers = new FinalizationRegistry(({ cb, data, hint }) => {
    scope(() => w.w_finalize(cb, data, hint));
  });
  const api = {
    napi_get_undefined: (out) => result(out, undefined),
    napi_get_null: (out) => result(out, null),
    napi_get_global: (out) => result(out, globalThis),
    napi_get_boolean: (v, out) => result(out, !!v),
    napi_create_object: (out) => result(out, {}),
    napi_create_array: (out) => result(out, []),
    napi_create_array_with_length: (n, out) => result(out, new Array(n >>> 0)),
    napi_create_double: (v, out) => result(out, v),
    napi_create_int32: (v, out) => result(out, v | 0),
    napi_create_uint32: (v, out) => result(out, v >>> 0),
    napi_create_string_utf8: (p, n, out) => result(out, string(p, n)),
    napi_create_error(code, message, out) {
      const e = new Error(values[message]);
      if (code) e.code = values[code];
      result(out, e);
    },
    napi_typeof(v, out) {
      const x = values[v];
      const types = { undefined: 0, boolean: 2, number: 3, string: 4, symbol: 5, object: 6, function: 7, bigint: 9 };
      write(out, x === null ? 1 : externals.has(x) ? 8 : types[typeof x]);
    },
    napi_get_value_bool(v, out) {
      if (typeof values[v] !== 'boolean') return 7;
      boolean(out, values[v]);
    },
    napi_get_value_double(v, out) {
      if (typeof values[v] !== 'number') return 6;
      mem().setFloat64(out, values[v], true);
    },
    napi_get_value_int32(v, out) {
      if (typeof values[v] !== 'number') return 6;
      write(out, values[v] | 0);
    },
    napi_get_value_uint32(v, out) {
      if (typeof values[v] !== 'number') return 6;
      write(out, values[v] >>> 0);
    },
    napi_get_value_string_utf8(v, p, cap, out) {
      if (typeof values[v] !== 'string') return 3;
      if (!p) { write(out, utf8Length(values[v])); return; }
      let n = 0;
      if (cap) {
        n = enc.encodeInto(values[v], bytes().subarray(p, p + cap - 1)).written;
        bytes()[p + n] = 0;
      }
      write(out, n);
    },
    napi_coerce_to_bool: (v, out) => result(out, !!values[v]),
    napi_coerce_to_string: (v, out) => result(out, `${values[v]}`),
    napi_strict_equals: (a, b, out) => boolean(out, values[a] === values[b]),
    napi_is_array: (v, out) => boolean(out, Array.isArray(values[v])),
    napi_get_array_length(v, out) {
      if (!Array.isArray(values[v])) return 8;
      write(out, values[v].length);
    },
    napi_get_element: (v, i, out) => result(out, values[v][i >>> 0]),
    napi_set_element: (v, i, x) => { Reflect.set(values[v], i >>> 0, values[x]); },
    napi_get_named_property: (v, p, out) => result(out, values[v][name(p)]),
    napi_set_named_property: (v, p, x) => { Reflect.set(values[v], name(p), values[x]); },
    napi_has_named_property: (v, p, out) => boolean(out, name(p) in values[v]),
    napi_get_property: (v, key, out) => result(out, values[v][values[key]]),
    napi_set_property: (v, key, x) => { Reflect.set(values[v], values[key], values[x]); },
    napi_get_property_names(v, out) {
      const keys = [];
      for (const key in values[v]) keys.push(key);
      result(out, keys);
    },
    napi_define_properties(v, n, p) {
      for (let i = 0; i < n; i++, p += 32) {
        const name = read(p) ? string(read(p)) : values[read(p + 4)];
        const flags = read(p + 24);
        Object.defineProperty(values[v], name, {
          value: read(p + 8) ? callback(read(p + 8), read(p + 28), name) : values[read(p + 20)],
          writable: !!(flags & 1), enumerable: !!(flags & 2), configurable: !!(flags & 4),
        });
      }
    },
    napi_create_reference(v, count, out) {
      // The shared core only requests strong references (refcount 1).
      if (count !== 1) throw new Error('unsupported weak reference');
      const id = freeRefs.length ? freeRefs.pop() : refs.length;
      refs[id] = values[v];
      write(out, id);
    },
    napi_delete_reference(id) { refs[id] = undefined; freeRefs.push(id); },
    napi_get_reference_value: (id, out) => result(out, refs[id]),
    napi_create_external(data, cb, hint, out) {
      const external = Object.create(null);
      externals.set(external, data);
      if (cb) finalizers.register(external, { cb, data, hint });
      result(out, external);
    },
    napi_get_value_external(v, out) {
      if (!externals.has(values[v])) return 1;
      write(out, externals.get(values[v]));
    },
    napi_create_function: (p, n, cb, data, out) => result(out, callback(cb, data, string(p, n))),
    napi_get_cb_info(info, argc, argv, self, data) {
      const frame = frames[info - 1];
      const cap = argc ? read(argc) : 0;
      for (let i = 0; argv && i < cap; i++) result(argv + 4 * i, frame.args[i]);
      write(argc, frame.args.length);
      if (self) result(self, frame.self);
      write(data, frame.data);
    },
    napi_call_function(self, fn, n, argv, out) {
      if (typeof values[fn] !== 'function') return 5;
      const args = new Array(n);
      for (let i = 0; i < n; i++) args[i] = values[read(argv + 4 * i)];
      if (!buffers.size) return void result(out, Reflect.apply(values[fn], values[self], args));
      syncToJS();
      try { result(out, Reflect.apply(values[fn], values[self], args)); }
      finally { syncToC(); }
    },
    napi_create_arraybuffer(n, data, out) {
      const buffer = new ArrayBuffer(n >>> 0);
      write(data, mirror(buffer));
      result(out, buffer);
    },
    napi_create_typedarray(type, n, ab, offset, out) {
      if (type !== 1) return 1;
      result(out, new Uint8Array(values[ab], offset, n));
    },
    napi_get_typedarray_info(v, type, length, data, ab, offset) {
      const x = values[v];
      if (!(x instanceof Uint8Array)) return 1;
      write(type, 1);
      write(length, x.length);
      write(data, mirror(x.buffer) + x.byteOffset);
      if (ab) result(ab, x.buffer);
      write(offset, x.byteOffset);
    },
    napi_throw: (v) => { fail(values[v]); },
    napi_throw_error(code, message) {
      const e = new Error(string(message));
      if (code) e.code = string(code);
      fail(e);
    },
    napi_throw_type_error(code, message) {
      const e = new TypeError(string(message));
      if (code) e.code = string(code);
      fail(e);
    },
    napi_is_exception_pending: (out) => boolean(out, pending),
    napi_get_and_clear_last_exception(out) {
      result(out, pending ? exception : undefined);
      pending = false;
      exception = undefined;
    },
  };
  // (fixed arity: no rest/spread arrays per call; extra arguments are undefined)
  const napi = Object.fromEntries(Object.entries(api).map(([key, fn]) => [key, (_env, a, b, c, d, e, f) => {
    try { return fn(a, b, c, d, e, f) ?? 0; }
    catch (err) { return fail(err); }
  }]));
  function finish(instance) {
    w = instance.exports;
    if (w._initialize) w._initialize();
    return scope(() => values[w.w_init(put({}))]);
  }
  const instance = instantiate({ napi });
  return instance && typeof instance.then === 'function' ? instance.then(finish) : finish(instance);
}
