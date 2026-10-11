/**
 * The web binding's native layer: what core.mjs calls `native`, the wasm
 * twin of the Node.js addon (bindings/node/src/*.c, docs/adr/0008): the same
 * functions with the same results, in JavaScript over the public C API of
 * tinyactors.wasm (the release's
 * tinyactors, tinyactors_runtime and tinyactors_datamodel_ecmascript in one
 * module; mise run web:build). There is no C of the binding's own: structs
 * are read and written at the wasm32 offsets of api.mjs (generated from the
 * API descriptions), a struct passed by value is a pointer to a copy and one
 * returned by value is written through a pointer given first (the wasm32 C
 * ABI), and the JavaScript datamodel's vtable entries are JavaScript
 * functions placed in the module's function table.
 *
 * Two "ecmascript" datamodels:
 *   'native'   the page's own JavaScript: variables and event data are JS
 *              values, expressions functions of the session's scope
 *   'quickjs'  the C datamodel in the module (QuickJS); this layer only
 *              notes which sessions changed and keeps their last variables
 *
 * createNative(instance) takes the instantiated module (it imports nothing).
 */
import { TYPES, CONSTANTS } from './api.mjs';

const K = Object.fromEntries(Object.entries(CONSTANTS).map(([name, value]) => [name.replace(/^TA_/, ''), value]));
const OK = 0;
const NIL = 0xffffffff;
const ERROR = { INVALID: CONSTANTS.TA_ERROR_INVALID, NO_MEMORY: CONSTANTS.TA_ERROR_NO_MEMORY, UNSUPPORTED: CONSTANTS.TA_ERROR_UNSUPPORTED, STATE: CONSTANTS.TA_ERROR_STATE, EXECUTION: CONSTANTS.TA_ERROR_EXECUTION };

/* offsets and sizes of the API's structs (wasm32) */
const OFFSETS = new Map();
/* { field: offset } of a struct, made once */
const offsets = (type) => {
  let o = OFFSETS.get(type);
  if (!o) OFFSETS.set(type, (o = Object.fromEntries(Object.entries(TYPES[type].fields).map(([field, [offset]]) => [field, offset]))));
  return o;
};
const size = (type) => TYPES[type].size;
const at = (type, field) => offsets(type)[field];

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/* a vtable entry's wasm signature from its C type: "ta_status (*)(void *, uint64_t, ...)" */
function signatureOf(ctype) {
  const match = /^(.*?)\s*\(\*\)\((.*)\)$/.exec(ctype);
  const wide = (t) => /^(uint64_t|int64_t|ta_payload|ta_time)$/.test(t.trim().replace(/^const\s+/, ''));
  const params = match[2].split(',').map((t) => t.trim()).filter((t) => t && t !== 'void');
  return { params: params.map((t) => (wide(t) ? 0x7e : 0x7f)), result: match[1].trim() === 'void' ? null : wide(match[1]) ? 0x7e : 0x7f };
}

/* A module importing `fns` with their signatures and exporting them: wasm
 * functions JavaScript can place in a table (the WebAssembly.Function
 * proposal is not everywhere). */
function wasmFunctions(entries) {
  const leb = (n) => {
    const out = [];
    do {
      let byte = n & 0x7f;
      n >>>= 7;
      if (n) byte |= 0x80;
      out.push(byte);
    } while (n);
    return out;
  };
  const name = (s) => [...leb(s.length), ...encoder.encode(s)];
  const section = (id, body) => [id, ...leb(body.length), ...body];
  const types = entries.flatMap(({ signature: s }) => [0x60, ...leb(s.params.length), ...s.params, ...(s.result ? [1, s.result] : [0])]);
  const imports = entries.flatMap((_, i) => [...name('f'), ...name(String(i)), 0, ...leb(i)]);
  const exports = entries.flatMap((_, i) => [...name(String(i)), 0, ...leb(i)]);
  const bytes = new Uint8Array([
    0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
    ...section(1, [...leb(entries.length), ...types]),
    ...section(2, [...leb(entries.length), ...imports]),
    ...section(7, [...leb(entries.length), ...exports]),
  ]);
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), { f: Object.fromEntries(entries.map((e, i) => [String(i), e.fn])) });
  return entries.map((_, i) => instance.exports[String(i)]);
}

/* ---- neutral values (docs/protocol.md, "Values"), written and read by the
 * JavaScript datamodel: JSON, Uint8Array as {"$bytes"}, an object with a
 * toJSON method as what it returns (a reference: {"$ref": address}), an
 * object with a key starting with "$" escaped as {"$": ...}. Undefined and
 * functions are left out of objects and are null elsewhere, BigInt is its
 * decimal text, non-finite numbers are null. Deeper than 128: not neutral. */

const base64 = (bytes) => {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};
const unbase64 = (text) => {
  const s = atob(text);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
};

class NotNeutral extends Error {}

function wireValue(v, depth) {
  if (depth > 128) throw new NotNeutral('too deep (a cycle?)');
  switch (typeof v) {
    case 'string':
      return JSON.stringify(v);
    case 'number':
      return Number.isFinite(v) ? JSON.stringify(v) : 'null';
    case 'boolean':
      return v ? 'true' : 'false';
    case 'bigint':
      return JSON.stringify(String(v));
    case 'object':
      break;
    default:
      return 'null';
  }
  if (v === null) return 'null';
  if (v instanceof Uint8Array) return `{"$bytes":${JSON.stringify(base64(v))}}`;
  if (typeof v.toJSON === 'function') {
    const r = v.toJSON();
    if (r && typeof r === 'object' && typeof r.$ref === 'string') return `{"$ref":${JSON.stringify(r.$ref)}}`;
    return wireValue(r, depth + 1);
  }
  if (Array.isArray(v)) return `[${v.map((e) => wireValue(e, depth + 1)).join(',')}]`;
  const keys = Object.keys(v);
  const parts = [];
  for (const k of keys) {
    const x = v[k];
    if (x === undefined || typeof x === 'function' || typeof x === 'symbol') continue;
    parts.push(`${JSON.stringify(k)}:${wireValue(x, depth + 1)}`);
  }
  const body = `{${parts.join(',')}}`;
  return keys.some((k) => k.startsWith('$')) ? `{"$":${body}}` : body;
}
const wire = (v) => wireValue(v, 0);

function unwire(v, refs, depth = 0) {
  if (depth > 512 || v === null || typeof v !== 'object') return v;
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) if (v[i] && typeof v[i] === 'object') v[i] = unwire(v[i], refs, depth + 1);
    return v;
  }
  const keys = Object.keys(v);
  if (keys.length === 1) {
    const inner = v[keys[0]];
    if (keys[0] === '$bytes' && typeof inner === 'string') {
      try {
        return unbase64(inner);
      } catch {
        return v;
      }
    }
    if (keys[0] === '$ref' && typeof inner === 'string') {
      try {
        return (refs && refs(inner)) || inner;
      } catch {
        return inner;
      }
    }
    if (keys[0] === '$' && inner && typeof inner === 'object') v = inner;
  }
  for (const k of Object.keys(v)) if (v[k] && typeof v[k] === 'object') v[k] = unwire(v[k], refs, depth + 1);
  return v;
}
/* neutral JSON text -> a JS value; undefined if it does not parse */
const valueOfWire = (text, refs) => {
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch {
    return undefined;
  }
  return unwire(parsed, refs);
};

export function createNative(instance) {
  const x = instance.exports;
  if (x._initialize) x._initialize(); /* the C library's constructors, once */
  const table = x.__indirect_function_table;

  /* ---- memory: views made again only after the memory grew (its old buffer
   * is then detached and its views have length 0) */
  let u8 = new Uint8Array(x.memory.buffer);
  let dv = new DataView(x.memory.buffer);
  const fresh = () => {
    if (!u8.byteLength) {
      u8 = new Uint8Array(x.memory.buffer);
      dv = new DataView(x.memory.buffer);
    }
  };
  const bytesAt = () => (fresh(), u8);
  const u32 = (p) => (fresh(), dv.getUint32(p, true));
  const i32 = (p) => (fresh(), dv.getInt32(p, true));
  const setU32 = (p, v) => (fresh(), dv.setUint32(p, v >>> 0, true));
  const u64 = (p) => (fresh(), dv.getBigUint64(p, true));
  const n64 = (p) => (fresh(), Number(dv.getBigUint64(p, true)));
  const setU64 = (p, v) => (fresh(), dv.setBigUint64(p, BigInt(v), true));
  const f64 = (p) => (fresh(), dv.getFloat64(p, true));
  const setF64 = (p, v) => (fresh(), dv.setFloat64(p, v, true));
  /* UTF-8 at p; short ASCII (names, ids) without a TextDecoder round trip */
  const text = (p, n) => {
    if (!n) return '';
    const b = bytesAt();
    if (n <= 32) {
      let s = '';
      for (let i = p; i < p + n; i++) {
        const c = b[i];
        if (c > 0x7f) return decoder.decode(b.subarray(p, p + n));
        s += String.fromCharCode(c);
      }
      return s;
    }
    return decoder.decode(b.subarray(p, p + n));
  };
  const copyOut = (p, n) => bytesAt().slice(p, p + n);
  /* a ta_string_view at p */
  const view = (p) => text(u32(p), u32(p + 4));
  const viewLength = (p) => u32(p + 4);

  /* blocks that outlive a call */
  const malloc = (n, align = 8) => {
    const p = x.ta_wasm_alloc(n || 1, align);
    if (!p) throw new Error('tinyactors: out of wasm memory');
    return p;
  };
  const free = (p) => p && x.ta_wasm_free(p, 0, 0);
  const allocBytes = (bytes) => {
    const p = malloc(bytes.length, 8);
    bytesAt().set(bytes, p);
    return p;
  };

  /* Scratch for one call: a bump arena in chunks, released to a mark (nested
   * calls — datamodel callbacks inside a run — take theirs above it). */
  const chunks = [];
  let chunk = -1;
  let top = 0;
  const CHUNK = 65536;
  const scratch = (n, align = 8) => {
    n = n || 1;
    for (;;) {
      if (chunk >= 0) {
        const c = chunks[chunk];
        const p = (c.base + top + align - 1) & -align;
        if (p + n <= c.base + c.size) {
          top = p + n - c.base;
          return p;
        }
      }
      chunk++;
      top = 0;
      if (chunk === chunks.length || chunks[chunk].size < n + align) {
        const sz = Math.max(CHUNK, n + align);
        if (chunk < chunks.length) free(chunks[chunk].base);
        chunks[chunk] = { base: malloc(sz, 8), size: sz };
      }
    }
  };
  const zeroed = (n, align = 8) => {
    const p = scratch(n, align);
    bytesAt().fill(0, p, p + n);
    return p;
  };
  const mark = () => [chunk, top];
  const release = ([c, t]) => {
    chunk = c;
    top = t;
  };
  /* f() with scratch released afterwards */
  const scoped = (f) => {
    const m = mark();
    try {
      return f();
    } finally {
      release(m);
    }
  };
  /* UTF-8 bytes of s in scratch: [pointer, length] */
  const utf8 = (s) => {
    s = String(s);
    const n = s.length;
    if (n <= 32) {
      /* short ASCII without a TextEncoder round trip */
      const p = scratch(n * 3 + 1, 1);
      const b = bytesAt();
      let i = 0;
      for (; i < n; i++) {
        const c = s.charCodeAt(i);
        if (c > 0x7f) break;
        b[p + i] = c;
      }
      if (i === n) return [p, n];
      return [p, encoder.encodeInto(s, b.subarray(p, p + n * 3)).written];
    }
    const p = scratch(n * 3 + 1, 1);
    const { written } = encoder.encodeInto(s, bytesAt().subarray(p, p + n * 3));
    return [p, written];
  };
  /* a ta_string_view of s at p (or a new one in scratch) */
  const putView = (p, s) => {
    if (s === undefined || s === null) {
      setU32(p, 0);
      setU32(p + 4, 0);
      return p;
    }
    const [q, n] = utf8(s);
    setU32(p, q);
    setU32(p + 4, n);
    return p;
  };
  const viewOf = (s) => putView(scratch(8, 4), s);
  const bytesIn = (bytes) => {
    const p = scratch(bytes.length, 8);
    bytesAt().set(bytes, p);
    return p;
  };
  /* a size_t / uint32 / uint64 out-parameter in scratch */
  const out32 = () => zeroed(4, 4);
  const out64 = () => zeroed(8, 8);
  const asBytes = (b) => (b instanceof Uint8Array ? b : ArrayBuffer.isView(b) ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength) : b instanceof ArrayBuffer ? new Uint8Array(b) : null);

  /* JavaScript functions as wasm functions in the module's table: their indices */
  const tableFunctions = (entries) => {
    const fns = wasmFunctions(entries);
    const base = table.grow(fns.length);
    fns.forEach((f, i) => table.set(base + i, f));
    return fns.map((_, i) => base + i);
  };

  /* ---- images: a ta_image over a copy of its bytes, freed with its handle */
  const imageMemory = new FinalizationRegistry(([bytes, image]) => {
    free(bytes);
    free(image);
  });
  class NativeImage {
    constructor(bytes) {
      this.length = bytes.length;
      this.bytes = allocBytes(bytes);
      this.image = malloc(size('ta_image'), 8);
      if (x.ta_image_open(this.bytes, bytes.length, this.image) !== OK) {
        free(this.bytes);
        free(this.image);
        throw new Error('not a valid tinyactors image');
      }
      imageMemory.register(this, [this.bytes, this.image]);
    }
  }
  const imageOf = (v) => {
    if (!(v instanceof NativeImage)) throw new TypeError('expected an image');
    return v;
  };
  const stringOfImage = (fn, ...args) =>
    scoped(() => {
      const r = scratch(8, 4);
      fn(r, ...args);
      return view(r);
    });
  const chartSource = (image, chart) => stringOfImage(x.ta_image_chart_source, image, chart);
  const hashText = (h) => h.toString(16).padStart(16, '0');
  const imageHash = (image) =>
    scoped(() => {
      const info = zeroed(size('ta_image_info'));
      setU32(info, size('ta_image_info'));
      x.ta_image_get_info(image, info);
      return u64(info + at('ta_image_info', 'content_hash'));
    });

  /* src="..." at compile time: the files passed along, by name (file: and ./
   * stripped), else by base name */
  const sourceName = (s) => {
    for (const prefix of ['file://', 'file:', './']) if (s.length > prefix.length && s.startsWith(prefix)) s = s.slice(prefix.length);
    return s;
  };
  let compileFiles = [];
  const [resolveIndex] = tableFunctions([
    {
      signature: { params: [0x7f, 0x7f, 0x7f, 0x7f], result: 0x7f },
      fn: (context, base, source, out) => {
        const want = sourceName(view(source));
        const baseName = want.slice(want.lastIndexOf('/') + 1);
        const found = compileFiles.find((f) => f.name === want) || compileFiles.find((f) => f.name === baseName);
        if (!found) return ERROR.INVALID;
        setU32(out, found.at);
        setU32(out + 4, found.nameLength);
        setU32(out + 8, found.at2);
        setU32(out + 12, found.textLength);
        return OK;
      },
    },
  ]);

  function compileAny(sources, files, system) {
    if (system ? typeof sources !== 'string' : !Array.isArray(sources))
      throw new TypeError(system ? 'compileSystem(manifest: string, files?: {name, xml}[])' : 'compile(sources: {name, xml}[], files?: {name, xml}[])');
    return scoped(() => {
      const list = system ? [{ name: 'system.xml', xml: sources }] : sources;
      const all = [...list, ...(files || [])];
      for (const s of all) if (typeof s.name !== 'string' || typeof s.xml !== 'string') throw new TypeError('compile: name and xml must be strings');
      const array = scratch(size('ta_source') * all.length, 4);
      compileFiles = all.slice(list.length).map((s, i) => {
        const p = array + size('ta_source') * (list.length + i);
        putView(p, s.name);
        putView(p + 8, s.xml);
        return { name: s.name, at: u32(p), nameLength: u32(p + 4), at2: u32(p + 8), textLength: u32(p + 12) };
      });
      list.forEach((s, i) => {
        putView(array + size('ta_source') * i, s.name);
        putView(array + size('ta_source') * i + 8, s.xml);
      });
      const options = zeroed(size('ta_compile_options'));
      setU32(options, size('ta_compile_options'));
      setU32(options + at('ta_compile_options', 'resolve'), resolveIndex);
      const result = out32();
      const status = system ? x.ta_compile_system(options, array, result) : x.ta_compile_charts(options, array, list.length, result);
      const compiled = u32(result);
      try {
        if (status !== OK) {
          let message = system ? 'system compilation failed:' : 'SCXML compilation failed:';
          const diagnostics = out32();
          const n = compiled ? x.ta_compile_diagnostics(compiled, diagnostics) : 0;
          for (let i = 0; i < n; i++) {
            const d = u32(diagnostics) + size('ta_diagnostic') * i;
            message += `\n  ${view(d + at('ta_diagnostic', 'file'))}:${u32(d + at('ta_diagnostic', 'line'))}:${u32(d + at('ta_diagnostic', 'column'))}: ${view(d + at('ta_diagnostic', 'message'))}`;
          }
          throw new Error(message);
        }
        const r = scratch(8, 4);
        x.ta_compile_image(r, compiled);
        return new NativeImage(copyOut(u32(r), u32(r + 4)));
      } finally {
        compileFiles = [];
        if (compiled) x.ta_compile_free(compiled);
      }
    });
  }

  /* ---- the pure functions */
  const pure = {
    constants: K,

    compile: (sources, files) => compileAny(sources, files, false),
    compileSystem: (manifest, files) => compileAny(manifest, files, true),

    /* ops: 1 open (a name), 2 attribute (a name and a value), 3 text (one), 4 close */
    writeXML(ops, strings) {
      if (!(ops instanceof Uint8Array) || !Array.isArray(strings)) throw new TypeError('writeXML(ops: Uint8Array, strings: string[])');
      return scoped(() => {
        const writer = out32();
        if (x.ta_xml_writer_new(0, writer) !== OK) throw new Error('writeXML: out of memory');
        const w = u32(writer);
        try {
          let next = 0;
          let status = OK;
          for (let i = 0; i < ops.length && status === OK; i++) {
            const m = mark();
            const op = ops[i];
            const needs = op === 1 || op === 3 ? 1 : op === 2 ? 2 : 0;
            if (op < 1 || op > 4 || next + needs > strings.length) status = ERROR.INVALID;
            else if (op === 1) status = x.ta_xml_writer_open(w, viewOf(strings[next++]));
            else if (op === 2) {
              const name = viewOf(strings[next++]);
              status = x.ta_xml_writer_attribute(w, name, viewOf(strings[next++]));
            } else if (op === 3) status = x.ta_xml_writer_text(w, viewOf(strings[next++]));
            else status = x.ta_xml_writer_close(w);
            release(m);
          }
          const xml = scratch(8, 4);
          if (status === OK) status = x.ta_xml_writer_finish(w, xml);
          if (status !== OK)
            throw new Error(
              status === ERROR.NO_MEMORY
                ? 'writeXML: out of memory'
                : 'writeXML: not a well-formed element tree (an element or attribute name that is not an XML name, a control character, or an attribute after content)',
            );
          return view(xml);
        } finally {
          x.ta_xml_writer_free(w);
        }
      });
    },

    openImage(bytes) {
      const b = asBytes(bytes);
      if (!b) throw new TypeError('openImage(bytes: Uint8Array)');
      return new NativeImage(b);
    },
    imageBytes: (image) => copyOut(imageOf(image).bytes, image.length),

    /* { hash, charts: [{ name, source, datamodel, states: [{ id, parent, kind }] }], actors: [{ name, chart }] } */
    imageInfo(handle) {
      const image = imageOf(handle).image;
      return scoped(() => {
        const info = zeroed(size('ta_image_info'));
        setU32(info, size('ta_image_info'));
        x.ta_image_get_info(image, info);
        const charts = [];
        for (let c = 0; c < u32(info + at('ta_image_info', 'chart_count')); c++) {
          const states = [];
          const state = zeroed(size('ta_state_info'));
          for (let s = 0, n = x.ta_image_state_count(image, c); s < n; s++) {
            setU32(state, size('ta_state_info'));
            if (x.ta_image_state_info(image, c, s, state) !== OK) break;
            const parent = u32(state + at('ta_state_info', 'parent'));
            states.push({ id: view(state + at('ta_state_info', 'id')), parent: parent === NIL ? -1 : parent, kind: u32(state + at('ta_state_info', 'kind')) });
          }
          charts.push({
            name: stringOfImage(x.ta_image_chart_name, image, c),
            source: chartSource(image, c),
            datamodel: stringOfImage(x.ta_image_chart_datamodel, image, c),
            states,
          });
        }
        const actors = [];
        for (let i = 0; i < u32(info + at('ta_image_info', 'boot_actor_count')); i++) {
          const name = zeroed(8, 4);
          const chart = out32();
          actors.push(x.ta_image_boot_actor(image, i, name, chart) === OK ? { name: view(name), chart: u32(chart) } : {});
        }
        return { hash: hashText(u64(info + at('ta_image_info', 'content_hash'))), charts, actors };
      });
    },

    /* { system, local, kind: 'session' | 'name', session: decimal, name, encoded } | null */
    parseAddress(textValue) {
      if (typeof textValue !== 'string') return null;
      return scoped(() => {
        const a = zeroed(size('ta_address'));
        if (x.ta_address_parse(viewOf(textValue), a) !== OK) return null;
        const o = { system: view(a + at('ta_address', 'system')), local: view(a + at('ta_address', 'local')) };
        if (u32(a + at('ta_address', 'kind')) === K.ADDRESS_SESSION) return { ...o, kind: 'session', session: String(u64(a + at('ta_address', 'session'))) };
        const name = a + at('ta_address', 'name');
        const capacity = viewLength(name) + 1;
        const decoded = scratch(capacity, 1);
        const n = x.ta_address_name_decode(name, decoded, capacity);
        return n ? { ...o, kind: 'name', name: text(decoded, n), encoded: view(name) } : null;
      });
    },
    formatAddress(system, local) {
      if (typeof system !== 'string') throw new TypeError('formatAddress(system: string, local)');
      const result = scoped(() => {
        if (typeof local === 'number' || typeof local === 'bigint') {
          const buffer = scratch(K.ADDRESS_SESSION_MAX, 1);
          const n = x.ta_address_format(viewOf(system), BigInt(local), buffer, K.ADDRESS_SESSION_MAX);
          return n ? text(buffer, n) : null;
        }
        const name = String(local);
        const capacity = system.length * 3 + name.length * 9 + 32;
        const buffer = scratch(capacity, 1);
        const n = name ? x.ta_address_format_name(viewOf(system), viewOf(name), buffer, capacity) : 0;
        return n ? text(buffer, n) : null;
      });
      if (!result) throw new TypeError('formatAddress: not a valid system id, session id or name');
      return result;
    },
    systemIDValid: (s) => typeof s === 'string' && scoped(() => !!x.ta_address_system_valid(viewOf(s))),

    frameHeader(length) {
      return scoped(() => {
        const p = scratch(K.FRAME_HEADER, 1);
        x.ta_frame_header(length >>> 0, p);
        return copyOut(p, K.FRAME_HEADER);
      });
    },
    frameReader(max) {
      max = max >>> 0 || K.FRAME_MAX;
      const reader = malloc(size('ta_frame_reader'), 8);
      setU32(reader, size('ta_frame_reader'));
      x.ta_frame_reader_init(reader, max);
      const frames = { reader, buffer: malloc(max, 8) };
      imageMemory.register(frames, [frames.reader, frames.buffer]);
      return frames;
    },
    /* [Uint8Array (a frame) | number (a refused frame's length)] */
    frameRead(frames, chunkBytes) {
      const b = asBytes(chunkBytes);
      if (!frames || !frames.reader || !b) throw new TypeError('frameRead(reader, chunk: Uint8Array)');
      return scoped(() => {
        const out = [];
        let p = bytesIn(b);
        let n = b.length;
        const event = out32();
        while (n) {
          const used = x.ta_frame_read(frames.reader, frames.buffer, p, n, event);
          p += used;
          n -= used;
          const length = u32(frames.reader + at('ta_frame_reader', 'length'));
          if (u32(event) === K.FRAME_READY) out.push(copyOut(frames.buffer, length));
          else if (u32(event) === K.FRAME_REFUSED) out.push(length);
        }
        return out;
      });
    },

    stepSpanID(session, macrostep, microstep) {
      return scoped(() => {
        const key = zeroed(size('ta_step_key'));
        setU64(key, BigInt(session));
        setU64(key + 8, BigInt(macrostep));
        setU32(key + 16, microstep >>> 0);
        const t = scratch(16, 1);
        x.ta_span_id_format(x.ta_step_key_span_id(key), t, 16);
        return text(t, 16);
      });
    },
    /* { traceID, parentSpanID, flags } or null */
    parseTraceparent(header) {
      if (typeof header !== 'string') return null;
      return scoped(() => {
        const context = zeroed(size('ta_trace_context'));
        if (x.ta_traceparent_parse(viewOf(header), context) !== OK) return null;
        return { traceID: traceID(context), parentSpanID: spanID(u64(context + at('ta_trace_context', 'parent'))), flags: u32(context + at('ta_trace_context', 'flags')) };
      });
    },
    formatTraceparent(trace, span, flags) {
      if (!/^[0-9a-fA-F]{32}$/.test(String(trace)) || !/^[0-9a-fA-F]{16}$/.test(String(span))) return null;
      return scoped(() => {
        const context = zeroed(size('ta_trace_context'));
        setU64(context, BigInt(`0x${trace.slice(0, 16)}`));
        setU64(context + 8, BigInt(`0x${trace.slice(16)}`));
        setU64(context + at('ta_trace_context', 'parent'), BigInt(`0x${span}`));
        setU32(context + at('ta_trace_context', 'flags'), flags >>> 0);
        return traceparentOf(context) || null;
      });
    },
    decodeTraces,
    metricsToOTLP: (metrics, resource, start, time) => metricsRequest(null, metrics, resource, start, time),
    /* the kinds' names, in core's order from 1 (ta_trace_kind_name) */
    traceKinds: Object.freeze(Array.from({ length: K.TRACE_KIND_COUNT - 1 }, (_, i) => stringOfImage(x.ta_trace_kind_name, i + 1))),
    /* snapshotContents(blob) -> { systemID, ecmascript, now, images: [image | null] } */
    snapshotContents(bytes) {
      const b = asBytes(bytes);
      return scoped(() => {
        const p = b ? bytesIn(b) : 0;
        const contents = zeroed(size('ta_host_snapshot_contents'));
        const C = offsets('ta_host_snapshot_contents');
        setU32(contents, size('ta_host_snapshot_contents'));
        if (!p || x.ta_host_snapshot_unpack(p, b.length, contents) !== OK) throw new TypeError('not a system snapshot (system.snapshot())');
        const images = [];
        const length = out32();
        for (let i = 0; i < u32(contents + C.image_count); i++) {
          const q = x.ta_host_snapshot_image(p, b.length, i, length);
          images.push(q ? new NativeImage(copyOut(q, u32(length))) : null);
        }
        return { systemID: view(contents + C.system_id), ecmascript: view(contents + C.datamodel), now: n64(contents + C.time), images };
      });
    },
  };

  const traceID = (p) => {
    const t = scratch(32, 1);
    x.ta_trace_id_format(p, t, 32);
    return text(t, 32);
  };
  const spanID = (id) => {
    const t = scratch(16, 1);
    x.ta_span_id_format(BigInt(id), t, 16);
    return text(t, 16);
  };
  const traceparentOf = (context) => {
    const t = scratch(K.TRACEPARENT_LENGTH, 1);
    const n = x.ta_traceparent_format(context, t, K.TRACEPARENT_LENGTH);
    return n ? text(t, n) : undefined;
  };

  /* ---- decoding OTLP: the API's spans and metrics (the addon's src/trace.c, src/metrics.c) */
  const fieldsSize = size('ta_otlp_fields');
  /* a copy of a ta_otlp_fields value in scratch (passed by value) */
  const fieldsCopy = (p) => {
    const q = scratch(fieldsSize, 8);
    bytesAt().copyWithin(q, p, p + fieldsSize);
    return q;
  };
  /* a value: string, boolean, number (a BigInt beyond 2^53), array, an object of a list, a Uint8Array */
  function decodedValue(v) {
    const kind = u32(v + at('ta_otlp_value', 'kind'));
    switch (kind) {
      case K.OTLP_VALUE_STRING:
        return view(v + at('ta_otlp_value', 'bytes'));
      case K.OTLP_VALUE_BOOL:
        return (fresh(), dv.getBigInt64(v + at('ta_otlp_value', 'integer'), true) !== 0n);
      case K.OTLP_VALUE_INT: {
        fresh();
        const i = dv.getBigInt64(v + at('ta_otlp_value', 'integer'), true);
        return i >= -9007199254740991n && i <= 9007199254740991n ? Number(i) : i;
      }
      case K.OTLP_VALUE_DOUBLE:
        return f64(v + at('ta_otlp_value', 'number'));
      case K.OTLP_VALUE_ARRAY: {
        const items = fieldsCopy(v + at('ta_otlp_value', 'items'));
        const list = [];
        const cursor = out32();
        const item = zeroed(size('ta_otlp_value'));
        while (x.ta_otlp_next_value(items, cursor, item) === 1) list.push(decodedValue(item));
        return list;
      }
      case K.OTLP_VALUE_LIST:
        return decodedAttributes(v + at('ta_otlp_value', 'items'));
      case K.OTLP_VALUE_BYTES: {
        const b = v + at('ta_otlp_value', 'bytes');
        return copyOut(u32(b), u32(b + 4));
      }
      default:
        return null;
    }
  }
  /* attributes as an object (`text`: values as text, as metrics have them) */
  function decodedAttributes(fieldsAt, text) {
    return scoped(() => {
      const fields = fieldsCopy(fieldsAt);
      const out = {};
      const cursor = out32();
      const key = zeroed(8, 4);
      const value = zeroed(size('ta_otlp_value'));
      while (x.ta_otlp_next_attribute(fields, cursor, key, value) === 1) {
        const v = decodedValue(value);
        out[view(key)] = text ? String(v) : v;
      }
      return out;
    });
  }
  const SPAN_KINDS = ['unspecified', 'internal', 'server', 'client', 'producer', 'consumer'];
  const STATUS_CODES = ['unset', 'ok', 'error'];
  /* ms and the BigInt ns of a time at p (ns since the epoch) */
  const timeOf = (o, ms, nano, p) => {
    const ns = u64(p);
    o[ms] = Number(ns / 1000n) / 1000;
    o[nano] = ns;
  };
  function decodeTraces(bytes) {
    const b = asBytes(bytes);
    if (!b) throw new TypeError('decodeTraces(bytes: Uint8Array)');
    return scoped(() => {
      const p = bytesIn(b);
      const cursor = zeroed(size('ta_otlp_cursor'));
      const span = zeroed(size('ta_otlp_span'));
      const S = offsets('ta_otlp_span');
      const spans = [];
      let got;
      while ((got = x.ta_otlp_next_span(p, b.length, cursor, span)) === 1) {
        const o = { traceID: traceID(span + S.trace), spanID: spanID(u64(span + S.span)) };
        const parent = u64(span + S.parent);
        if (parent) o.parentSpanID = spanID(parent);
        o.name = view(span + S.name);
        o.kind = SPAN_KINDS[u32(span + S.kind)] || 'unspecified';
        timeOf(o, 'startTime', 'startTimeUnixNano', span + S.start);
        timeOf(o, 'endTime', 'endTimeUnixNano', span + S.end);
        o.attributes = decodedAttributes(span + S.attributes);
        o.events = scoped(() => {
          const events = [];
          const list = fieldsCopy(span + S.events);
          const c = out32();
          const e = zeroed(size('ta_otlp_event'));
          while (x.ta_otlp_next_event(list, c, e) === 1) {
            const event = { name: view(e + at('ta_otlp_event', 'name')) };
            timeOf(event, 'time', 'timeUnixNano', e + at('ta_otlp_event', 'time'));
            event.attributes = decodedAttributes(e + at('ta_otlp_event', 'attributes'));
            events.push(event);
          }
          return events;
        });
        o.links = scoped(() => {
          const links = [];
          const list = fieldsCopy(span + S.links);
          const c = out32();
          const l = zeroed(size('ta_otlp_link'));
          while (x.ta_otlp_next_link(list, c, l) === 1)
            links.push({ traceID: traceID(l + at('ta_otlp_link', 'trace')), spanID: spanID(u64(l + at('ta_otlp_link', 'span'))), attributes: decodedAttributes(l + at('ta_otlp_link', 'attributes')) });
          return links;
        });
        const code = STATUS_CODES[u32(span + S.status)] || 'unset';
        o.status = viewLength(span + S.status_message) ? { code, message: view(span + S.status_message) } : { code };
        o.resource = { attributes: decodedAttributes(span + S.resource) };
        o.scope = { name: view(span + S.scope_name), version: view(span + S.scope_version), attributes: decodedAttributes(span + S.scope_attributes) };
        spans.push(o);
      }
      if (got < 0) throw new Error('decodeTraces: malformed OTLP protobuf');
      return spans;
    });
  }
  const doubles = (p, counts) => {
    const n = u32(p + 4) >>> 3;
    const base = u32(p);
    const list = [];
    for (let i = 0; i < n; i++) list.push(counts ? n64(base + 8 * i) : f64(base + 8 * i));
    return list;
  };
  const METRIC_KINDS = ['', 'sum', 'gauge', 'histogram'];
  /* a metrics request as the API's metrics */
  function metricsValue(bytes) {
    return scoped(() => {
      const p = bytesIn(bytes);
      const cursor = zeroed(size('ta_otlp_cursor'));
      const metric = zeroed(size('ta_otlp_metric_view'));
      const M = offsets('ta_otlp_metric_view');
      const P = offsets('ta_otlp_point_view');
      const metrics = [];
      let got;
      while ((got = x.ta_otlp_next_metric(p, bytes.length, cursor, metric)) === 1) {
        const kind = u32(metric + M.kind);
        const o = { name: view(metric + M.name), unit: view(metric + M.unit), kind: METRIC_KINDS[kind] || 'gauge', points: [] };
        const m = mark();
        const list = fieldsCopy(metric + M.points);
        const c = out32();
        const point = zeroed(size('ta_otlp_point_view'));
        while ((got = x.ta_otlp_next_point(list, c, kind, point)) === 1) {
          const q = { attributes: decodedAttributes(point + P.attributes, true) };
          if (kind === K.OTLP_METRIC_HISTOGRAM) {
            q.count = n64(point + P.count);
            q.sum = f64(point + P.sum);
            if (u32(point + P.has_min_max)) {
              q.min = f64(point + P.min);
              q.max = f64(point + P.max);
            }
            q.bounds = doubles(point + P.bounds, false);
            q.buckets = doubles(point + P.buckets, true);
            q.exemplars = scoped(() => {
              const exemplars = [];
              const xs = fieldsCopy(point + P.exemplars);
              const xc = out32();
              const e = zeroed(size('ta_otlp_exemplar'));
              const E = offsets('ta_otlp_exemplar');
              while (x.ta_otlp_next_exemplar(xs, xc, e) === 1) {
                const value = f64(e + E.value);
                let bucket = q.bounds.findIndex((bound) => value <= bound);
                if (bucket < 0) bucket = q.bounds.length;
                exemplars.push({ traceID: traceID(e + E.trace), spanID: spanID(u64(e + E.span)), time: n64(e + E.time) / 1e6, value, bucket });
              }
              return exemplars;
            });
          } else {
            fresh();
            q.value = u32(point + P.flags) & K.OTLP_POINT_DOUBLE ? f64(point + P.number) : Number(dv.getBigInt64(point + P.integer, true));
          }
          o.points.push(q);
        }
        release(m);
        if (got < 0) break;
        metrics.push(o);
      }
      if (got < 0) throw new Error('metrics: malformed OTLP protobuf');
      return metrics;
    });
  }

  /* ---- OTLP encoders */
  const otlpConfig = (fields) => {
    const config = zeroed(size('ta_otlp_config'));
    setU32(config, size('ta_otlp_config'));
    for (const [k, v] of Object.entries(fields)) setU32(config + at('ta_otlp_config', k), v);
    return config;
  };
  /* a new encoder for `fields` (malloc'd: it outlives the call), or 0 */
  const otlpNew = (fields) =>
    scoped(() => {
      const config = otlpConfig(fields);
      const length = out32();
      if (x.ta_otlp_layout(config, length) !== OK) return 0;
      const memory = malloc(u32(length), 8);
      const otlp = out32();
      if (x.ta_otlp_init(memory, u32(length), config, otlp) !== OK) {
        free(memory);
        return 0;
      }
      return { otlp: u32(otlp), memory };
    });
  /* the encoder moved into a bigger block (fields), or 0 (the old one kept) */
  const otlpRelocate = (encoder, fields) =>
    scoped(() => {
      const config = otlpConfig(fields);
      const length = out32();
      if (x.ta_otlp_layout(config, length) !== OK) return 0;
      const memory = malloc(u32(length), 8);
      const moved = out32();
      if (x.ta_otlp_relocate(encoder.otlp, memory, u32(length), config, moved) !== OK) {
        free(memory);
        return 0;
      }
      free(encoder.memory);
      return { otlp: u32(moved), memory };
    });
  /* an encoder's ta_otlp_stats, as { field: value } */
  const otlpStats = (otlp) =>
    scoped(() => {
      const stats = zeroed(size('ta_otlp_stats'));
      setU32(stats, size('ta_otlp_stats'));
      x.ta_otlp_stats_get(otlp, stats);
      return Object.fromEntries(Object.entries(offsets('ta_otlp_stats')).map(([field, offset]) => [field, u32(stats + offset)]));
    });
  /* attributes ({ key: value } or [[key, value], ...]) as ta_otlp_attribute[] in scratch */
  const attributesIn = (pairs) => {
    const list = Array.isArray(pairs) ? pairs : pairs && typeof pairs === 'object' ? Object.entries(pairs) : [];
    const p = scratch(size('ta_otlp_attribute') * (list.length || 1), 4);
    list.forEach(([k, v], i) => {
      putView(p + size('ta_otlp_attribute') * i, String(k));
      putView(p + size('ta_otlp_attribute') * i + 8, String(v));
    });
    return [p, list.length];
  };
  const hexPair = (s) => (/^[0-9a-fA-F]{32}$/.test(s) ? [BigInt(`0x${s.slice(0, 16)}`), BigInt(`0x${s.slice(16)}`)] : null);
  const hex64 = (s) => (/^[0-9a-fA-F]{16}$/.test(s) ? BigInt(`0x${s}`) : null);

  /* a metric of the API ({ name, unit, kind: "sum" | "gauge" | "histogram", points }) onto an
   * encoder; ERROR.INVALID when it is malformed */
  function metricAdd(otlp, metric, start, time) {
    const kind = metric && typeof metric === 'object' ? METRIC_KINDS.indexOf(metric.kind) : -1;
    if (kind < 1 || typeof metric.name !== 'string' || !Array.isArray(metric.points)) return ERROR.INVALID;
    return scoped(() => {
      const points = Array.isArray(metric.points) ? metric.points : [];
      const PT = offsets('ta_otlp_point');
      const array = zeroed(size('ta_otlp_point') * (points.length || 1));
      points.forEach((point, i) => {
        const p = array + size('ta_otlp_point') * i;
        const [attributes, count] = attributesIn(point.attributes);
        setU32(p + PT.attributes, attributes);
        setU32(p + PT.attribute_count, count);
        if (Number.isSafeInteger(point.value)) {
          fresh();
          dv.setBigInt64(p + PT.integer, BigInt(Math.trunc(point.value || 0)), true);
        } else {
          setU32(p + PT.flags, K.OTLP_POINT_DOUBLE);
          setF64(p + PT.number, Number(point.value || 0));
        }
        setU64(p + PT.count, BigInt(Math.trunc(point.count || 0)));
        setF64(p + PT.sum, Number(point.sum || 0));
        setF64(p + PT.min, Number(point.min || 0));
        setF64(p + PT.max, Number(point.max || 0));
        const buckets = Array.isArray(point.buckets) ? point.buckets : [];
        const bounds = Array.isArray(point.bounds) ? point.bounds : [];
        const bucketArray = scratch(8 * (buckets.length || 1), 8);
        buckets.forEach((b, k) => setU64(bucketArray + 8 * k, BigInt(Math.trunc(b))));
        const boundArray = scratch(8 * (bounds.length || 1), 8);
        bounds.forEach((b, k) => setF64(boundArray + 8 * k, b));
        setU32(p + PT.buckets, bucketArray);
        setU32(p + PT.bounds, boundArray);
        setU32(p + PT.bucket_count, buckets.length && bounds.length + 1 === buckets.length ? buckets.length : 0);
        const exemplars = (Array.isArray(point.exemplars) ? point.exemplars : []).filter((e) => e && hexPair(String(e.traceID)) && hex64(String(e.spanID)) !== null);
        const E = offsets('ta_otlp_exemplar');
        const exemplarArray = zeroed(size('ta_otlp_exemplar') * (exemplars.length || 1));
        exemplars.forEach((e, k) => {
          const q = exemplarArray + size('ta_otlp_exemplar') * k;
          const [high, low] = hexPair(String(e.traceID));
          setU64(q + E.trace, high);
          setU64(q + E.trace + 8, low);
          setU64(q + E.span, hex64(String(e.spanID)));
          setU64(q + E.time, BigInt(Math.trunc((e.time || 0) * 1e6)));
          setF64(q + E.value, Number(e.value || 0));
        });
        setU32(p + PT.exemplars, exemplarArray);
        setU32(p + PT.exemplar_count, exemplars.length);
      });
      const M = offsets('ta_otlp_metric');
      const m = zeroed(size('ta_otlp_metric'));
      putView(m + M.name, metric.name === undefined ? '' : String(metric.name));
      putView(m + M.unit, metric.unit === undefined ? '' : String(metric.unit));
      setU32(m + M.kind, kind);
      setU32(m + M.points, array);
      setU32(m + M.point_count, points.length);
      setU64(m + M.start_ns, start);
      setU64(m + M.time_ns, time);
      return x.ta_otlp_add_metric(otlp, m);
    });
  }

  const METRIC_FIELDS = (metricBytes) => ({ open_spans: 1, staging_bytes: K.OTLP_STAGING_BYTES_MIN, log_bytes: 1024, scopes: 1, metric_bytes: metricBytes });
  /* the request of `metrics` and, with a system, its stats; times in ms since the epoch */
  function metricsRequest(sys, metrics, resource, startMs, timeMs) {
    const start = startMs > 0 ? BigInt(Math.trunc(startMs * 1e6)) : 0n;
    const time = timeMs > 0 ? BigInt(Math.trunc(timeMs * 1e6)) : 0n;
    let encoder = otlpNew(METRIC_FIELDS(262144));
    if (!encoder) throw new Error('no memory for the metrics request');
    const grow = () => {
      const capacity = otlpStats(encoder.otlp).metric_capacity;
      const moved = capacity < K.OTLP_STAGING_BYTES_MAX / 2 ? otlpRelocate(encoder, METRIC_FIELDS(capacity * 2)) : 0;
      if (!moved) throw new Error('no memory for the metrics request');
      encoder = moved;
    };
    try {
      scoped(() => {
        const [attributes, count] = attributesIn(resource);
        x.ta_otlp_set_resource(encoder.otlp, attributes, count);
      });
      if (sys) {
        scoped(() => {
          const input = zeroed(size('ta_otlp_stats_input'));
          const I = offsets('ta_otlp_stats_input');
          setU32(input, size('ta_otlp_stats_input'));
          const chartCount = x.ta_host_chart_stats(sys.host, 0, 0);
          const charts = zeroed(size('ta_chart_stats') * (chartCount || 1));
          for (let i = 0; i < chartCount; i++) setU32(charts + size('ta_chart_stats') * i, size('ta_chart_stats'));
          x.ta_host_chart_stats(sys.host, charts, chartCount);
          const segments = x.ta_host_segment_count(sys.host);
          const systems = zeroed(size('ta_system_stats') * (segments || 1));
          let systemCount = 0;
          for (let k = 0; k < segments; k++) {
            const system = x.ta_host_system(sys.host, k);
            if (!system) continue;
            const q = systems + size('ta_system_stats') * systemCount++;
            setU32(q, size('ta_system_stats'));
            x.ta_system_stats_get(system, q);
          }
          const hostStats = zeroed(size('ta_host_stats'));
          setU32(hostStats, size('ta_host_stats'));
          x.ta_host_stats_get(sys.host, hostStats);
          setU32(input + I.chart_count, chartCount);
          setU32(input + I.charts, charts);
          setU32(input + I.systems, systems);
          setU32(input + I.system_count, systemCount);
          setU32(input + I.host, hostStats);
          setU64(input + I.ticks_per_second, 1000n); /* the bindings' clock is in ms */
          setU64(input + I.start_ns, start);
          setU64(input + I.time_ns, time);
          while (x.ta_otlp_add_stats(encoder.otlp, input) === ERROR.NO_MEMORY) grow();
        });
        while (sys.otlp && x.ta_otlp_add_span_metrics(encoder.otlp, sys.otlp.otlp, start, time) === ERROR.NO_MEMORY) grow();
      }
      for (const metric of Array.isArray(metrics) ? metrics : []) {
        let status;
        while ((status = metricAdd(encoder.otlp, metric, start, time)) === ERROR.NO_MEMORY) grow();
        if (status !== OK) throw new TypeError('metricsToOTLP: a metric needs a name, a kind ("sum", "gauge" or "histogram") and points');
      }
      return scoped(() => {
        const n = x.ta_otlp_metrics_size(encoder.otlp);
        const p = scratch(n, 8);
        if (n) x.ta_otlp_put_metrics(encoder.otlp, p, n);
        return copyOut(p, n);
      });
    } finally {
      free(encoder.memory);
    }
  }

  return { ...pure, ...systems({ x, K, u8: bytesAt, u32, i32, setU32, u64, n64, setU64, text, copyOut, view, viewLength, malloc, free, allocBytes, scratch, zeroed, mark, release, scoped, utf8, putView, viewOf, bytesIn, out32, out64, asBytes, tableFunctions, NativeImage, imageOf, imageHash, chartSource, sourceName, stringOfImage, traceparentOf, otlpNew, otlpRelocate, otlpStats, attributesIn, metricsRequest, metricsValue, hashText }) };
}

/* ---- systems: a ta_host, its datamodel, its run loop ---------------------------------------- */

function systems(m) {
  const { x, K, u32, i32, setU32, u64, n64, setU64, text, copyOut, view, malloc, free, scratch, zeroed, mark, release, scoped, putView, viewOf, bytesIn, out32, out64, asBytes, tableFunctions, NativeImage, imageOf, imageHash, chartSource, sourceName, stringOfImage, traceparentOf, otlpNew, otlpRelocate, otlpStats, attributesIn, metricsRequest, metricsValue, hashText, viewLength } = m;
  const bytesAt = m.u8;
  const EFFECT = offsets('ta_host_effect');
  const EFFECT_SIZE = size('ta_host_effect');
  const EVENT_IN = offsets('ta_event_in');
  const IO_PROCESSORS_MAX = 16;
  const RUN_BUDGET = 10000;
  const RUN_ROUNDS = 16;
  const EVENT_TYPES = ['platform', 'internal', 'external'];

  /* every system's state, by the datamodel pointer its vtable entries get */
  const live = new Map();
  let nextKey = 1;
  /* the system whose datamodel the core is calling (the vtable's d is its key) */
  const of = (d) => live.get(d);

  const serialOf = (sessionid) => {
    let serial = 0;
    for (const ch of sessionid) if (ch >= '0' && ch <= '9') serial = serial * 10 + (ch.charCodeAt(0) - 48);
    return serial;
  };
  const markChanged = (sys, holder, serial) => {
    if (holder.stamp === sys.pump || !serial) return;
    holder.stamp = sys.pump;
    sys.changed.push(serial);
  };
  const graveAdd = (sys, serial, context) => {
    if (sys.keepQuiet || context === undefined) return;
    sys.graves.push([serial, context]);
  };
  const failure = (sys, serial, error) => sys.failures.push([serial, error]);

  /* ---- the JavaScript "ecmascript" datamodel (values are JS values) */
  const instanceAt = (sys, handle) => {
    const i = sys.instances[Number(handle) - 1];
    return i && i.live ? i : undefined;
  };
  const payloadNew = (sys, v) => {
    let i;
    if (sys.freePayload) {
      i = sys.freePayload - 1;
      sys.freePayload = sys.payloadNext[i];
    } else i = sys.payloads.length;
    sys.payloads[i] = v;
    sys.payloadRefs[i] = 1;
    sys.livePayloads++;
    return i + 1;
  };
  const payloadGet = (sys, p) => {
    p = Number(p);
    return p && sys.payloadRefs[p - 1] ? sys.payloads[p - 1] : undefined;
  };
  const retain = (sys, p) => {
    p = Number(p);
    if (p && sys.payloadRefs[p - 1]) sys.payloadRefs[p - 1]++;
  };
  const releasePayload = (sys, p) => {
    p = Number(p);
    if (!p || !sys.payloadRefs[p - 1]) return;
    if (--sys.payloadRefs[p - 1] === 0) {
      sys.payloads[p - 1] = undefined;
      sys.payloadNext[p - 1] = sys.freePayload;
      sys.freePayload = p;
      sys.livePayloads--;
    }
  };
  const hidden = (o, name, v) => Object.defineProperty(o, name, { value: v, configurable: true, writable: false, enumerable: false });
  /* _event: read-only to the chart, set by the binding on each event (a slot
   * the getter reads, not a property defined again per event) */
  const EVENT = Symbol('event');
  function eventGetter() {
    return this[EVENT];
  }
  const setEvent = (scope, e) => {
    scope[EVENT] = e;
  };
  const scopeNew = (sys, sessionid, name, io) => {
    const scope = {};
    hidden(scope, '_sessionid', sessionid);
    hidden(scope, '_name', name);
    hidden(scope, '_ioprocessors', io);
    hidden(scope, 'In', sys.inFunction);
    Object.defineProperty(scope, EVENT, { value: undefined, writable: true, enumerable: false });
    Object.defineProperty(scope, '_event', { get: eventGetter, configurable: true, enumerable: false });
    return scope;
  };
  const SV = offsets('ta_datamodel_system_variables');
  const ENTRY = size('ta_io_processor_entry');
  const svScope = (sys, sv) => {
    const io = {};
    const entries = u32(sv + SV.io_processors);
    for (let i = 0; i < u32(sv + SV.io_processor_count); i++) io[view(entries + ENTRY * i)] = { location: view(entries + ENTRY * i + 8) };
    return scopeNew(sys, view(sv + SV.sessionid), view(sv + SV.name), io);
  };
  const instanceNew = (sys, chart, serial, scope) => {
    let i;
    if (sys.freeInstance) {
      i = sys.freeInstance - 1;
      sys.freeInstance = sys.instances[i].nextFree;
    } else i = sys.instances.length;
    const instance = { live: true, chart, serial, scope, input: undefined, stamp: 0, outside: 0, string: 0, nextFree: 0 };
    sys.instances[i] = instance;
    sys.liveInstances++;
    return [instance, i + 1];
  };
  const instanceFree = (sys, handle) => {
    const instance = instanceAt(sys, handle);
    if (!instance) return;
    free(instance.string);
    instance.live = false;
    instance.scope = instance.input = undefined;
    instance.nextFree = sys.freeInstance;
    sys.freeInstance = Number(handle);
    sys.liveInstances--;
  };
  const expressionInfo = (chart, e) =>
    scoped(() => {
      const info = zeroed(size('ta_expression_info'));
      if (x.ta_image_expression(chart.image, chart.chart, e, info) !== OK) return null;
      return { kind: u32(info + at('ta_expression_info', 'kind')), source: view(info + at('ta_expression_info', 'source')) };
    });
  const dataInfo = (image, chart, i) =>
    scoped(() => {
      const info = zeroed(size('ta_data_info'));
      if (x.ta_image_data(image, chart, i, info) !== OK) return null;
      return { id: view(info + at('ta_data_info', 'id')), expression: u32(info + at('ta_data_info', 'expression')) };
    });
  /* expression e's function with (scope[, value]); undefined on an exception (recorded) */
  const NONE = Symbol('none');
  const evaluate = (sys, instance, e, value = NONE) => {
    const f = instance.chart.functions[e];
    if (!f) {
      const info = expressionInfo(instance.chart, e);
      failure(sys, instance.serial, new Error(info ? `no implementation for the expression \`${info.source.slice(0, 500)}\`` : `unknown expression ${e}`));
      return NONE;
    }
    const outer = sys.evaluating;
    sys.evaluating = instance;
    try {
      return value === NONE ? f(instance.scope) : f(instance.scope, value);
    } catch (error) {
      failure(sys, instance.serial, error);
      return NONE;
    } finally {
      sys.evaluating = outer;
    }
  };
  const chartInfo = (chartPointer) => {
    const image = u32(chartPointer + at('ta_datamodel_chart', 'image'));
    const chart = u32(chartPointer + at('ta_datamodel_chart', 'chart'));
    return { image, chart, hash: imageHash(image), count: x.ta_image_expression_count(image, chart) };
  };
  /* the string an eval_string / log result points at, until the next one */
  const putString = (instance, s, out) => {
    free(instance.string);
    const bytes = encoder.encode(s);
    instance.string = malloc(bytes.length || 1, 1);
    m.u8().set(bytes, instance.string);
    setU32(out, instance.string);
    setU32(out + 4, bytes.length);
    return OK;
  };
  /* <foreach item>/index declare their variable when it is not declared (SCXML 4.6) */
  const foreachAssign = (sys, instance, location, value) => {
    const info = expressionInfo(instance.chart, location);
    if (!info || !/^[A-Za-z_$][\w$]*$/.test(info.source)) return evaluate(sys, instance, location, value) === NONE ? ERROR.EXECUTION : OK;
    instance.scope[info.source] = value;
    return OK;
  };
  /* hibernation and export: {"v": variables, "e": _event} as neutral values */
  const scopeWire = (sys, scope) => {
    const parts = [];
    for (const k of Object.keys(scope)) {
      const v = scope[k];
      if (v === undefined) continue;
      if (typeof v === 'function' || typeof v === 'symbol') {
        sys.error = `variable '${k.slice(0, 200)}' cannot be stored: it holds a function`;
        return null;
      }
      try {
        parts.push(`${JSON.stringify(k)}:${wireValue(v, 1)}`);
      } catch {
        sys.error = `variable '${k.slice(0, 200)}' cannot be stored: it is not a neutral value (a cycle?)`;
        return null;
      }
    }
    let event = 'null';
    try {
      event = wireValue(scope._event, 1);
    } catch {
      /* stays null */
    }
    return `{"v":{${parts.join(',')}},"e":${event}}`;
  };
  /* write text to a (buffer, capacity, length*) out-parameter triple */
  const writeOut = (s, buffer, capacity, length) => {
    const bytes = encoder.encode(s);
    setU32(length, bytes.length);
    if (bytes.length > capacity) return ERROR.NO_MEMORY;
    m.u8().set(bytes, buffer);
    return OK;
  };
  const eventObject = (sys, ev) => {
    const E = offsets('ta_event_view');
    const type = u32(ev + E.type);
    const e = { name: view(ev + E.name), type: EVENT_TYPES[type < 3 ? type : 0] };
    for (const field of ['sendid', 'origin', 'origintype', 'invokeid', 'raw']) if (u32(ev + E[field] + 4)) e[field] = view(ev + E[field]);
    const data = u64(ev + E.data);
    if (data) e.data = payloadGet(sys, data);
    return e;
  };

  /* The vtable entries, one set for every system (d says which). */
  const NATIVE = {
    chart_attach(d, chartPointer, context) {
      const sys = of(d);
      const chart = chartInfo(chartPointer);
      chart.functions = new Array(chart.count);
      const texts = new Array(chart.count);
      const kinds = new Array(chart.count);
      for (let e = 0; e < chart.count; e++) {
        const info = expressionInfo(chart, e);
        if (info) [texts[e], kinds[e]] = [info.source, info.kind];
      }
      try {
        const fns = sys.callbacks.resolve(chartSource(chart.image, chart.chart), texts, kinds);
        for (let e = 0; e < chart.count; e++) if (typeof fns[e] === 'function') chart.functions[e] = fns[e];
      } catch (error) {
        failure(sys, 0, error); /* unresolved expressions fail when evaluated */
      }
      const key = sys.nextChart++;
      sys.charts.set(key, chart);
      setU32(context, key);
      return OK;
    },
    chart_detach(d, context) {
      const sys = of(d);
      if (sys) sys.charts.delete(context);
    },
    create(d, context, sv, init, out) {
      const sys = of(d);
      const serial = serialOf(view(sv + SV.sessionid));
      const [instance, handle] = instanceNew(sys, sys.charts.get(context), serial, svScope(sys, sv));
      if (init) instance.input = payloadGet(sys, init);
      releasePayload(sys, init); /* the init payload is ours now */
      markChanged(sys, instance, serial);
      setU64(out, handle);
      return OK;
    },
    destroy(d, handle) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      if (!instance) return;
      graveAdd(sys, instance.serial, instance.scope);
      instanceFree(sys, handle);
    },
    /* <data id expr>: the spawn's input overrides it (a property of that name) */
    init_data(d, handle, outside, declaration) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      if (!instance) return ERROR.EXECUTION;
      instance.outside = outside;
      try {
        const data = dataInfo(instance.chart.image, instance.chart.chart, declaration);
        if (!data) return ERROR.EXECUTION;
        let v = instance.input && typeof instance.input === 'object' ? instance.input[data.id] : undefined;
        if (v === undefined) v = data.expression === NIL ? undefined : evaluate(sys, instance, data.expression);
        if (v === NONE) return ERROR.EXECUTION;
        instance.scope[data.id] = v;
        return OK;
      } finally {
        instance.outside = 0;
      }
    },
    bind_event(d, handle, outside, ev) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      if (!instance) return ERROR.EXECUTION;
      setEvent(instance.scope, eventObject(sys, ev));
      markChanged(sys, instance, instance.serial);
      return OK;
    },
    eval_condition(d, handle, outside, e, out) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const v = evaluate(sys, instance, e);
        if (v === NONE) return ERROR.EXECUTION;
        setU32(out, v ? 1 : 0);
        return OK;
      });
    },
    eval_string(d, handle, outside, e, out) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const v = evaluate(sys, instance, e);
        if (v === NONE) return ERROR.EXECUTION;
        try {
          return putString(instance, String(v), out);
        } catch (error) {
          failure(sys, instance.serial, error);
          return ERROR.EXECUTION;
        }
      });
    },
    log(d, handle, outside, e, out) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const v = evaluate(sys, instance, e);
        if (v === NONE) return ERROR.EXECUTION;
        let s;
        try {
          s = v && typeof v === 'object' ? JSON.stringify(v) : undefined;
        } catch {
          s = undefined;
        }
        try {
          return putString(instance, typeof s === 'string' ? s : String(v), out);
        } catch (error) {
          failure(sys, instance.serial, error);
          return ERROR.EXECUTION;
        }
      });
    },
    /* location expressions resolve to setters: f(scope, value) */
    assign(d, handle, outside, location, value) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const v = evaluate(sys, instance, value);
        if (v === NONE || evaluate(sys, instance, location, v) === NONE) return ERROR.EXECUTION;
        return OK;
      });
    },
    assign_string(d, handle, location, value) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      if (!instance) return ERROR.EXECUTION;
      return evaluate(sys, instance, location, view(value)) === NONE ? ERROR.EXECUTION : OK;
    },
    script(d, handle, outside, e) {
      return withInstance(d, handle, outside, (sys, instance) => (evaluate(sys, instance, e) === NONE ? ERROR.EXECUTION : OK));
    },
    foreach_begin(d, handle, outside, array, iterator, n) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const v = evaluate(sys, instance, array);
        if (v === NONE) return ERROR.EXECUTION;
        if (!Array.isArray(v)) {
          failure(sys, instance.serial, new TypeError('<foreach array>: not an array'));
          return ERROR.EXECUTION;
        }
        const copy = Array.from(v); /* a shallow copy */
        setU32(n, copy.length);
        setU64(iterator, payloadNew(sys, copy));
        return OK;
      });
    },
    foreach_item(d, handle, iterator, i, item, index) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      if (!instance) return ERROR.EXECUTION;
      if (foreachAssign(sys, instance, item, payloadGet(sys, iterator)[i]) !== OK) return ERROR.EXECUTION;
      if (index >>> 0 !== NIL && foreachAssign(sys, instance, index, i) !== OK) return ERROR.EXECUTION;
      return OK;
    },
    foreach_end(d, handle, iterator) {
      releasePayload(of(d), iterator);
    },
    /* namelist and <param>s: an object of their values; <content>: its value; a
     * trace's value (TA_CAPTURE_VALUE): read with no effect the chart could see */
    capture(d, handle, outside, capture, out) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const C = offsets('ta_datamodel_capture');
        const { chart } = instance;
        const content = u32(capture + C.content);
        let v;
        if (u32(capture + C.kind) === K.CAPTURE_VALUE) {
          const info = expressionInfo(chart, content);
          if (!info) return ERROR.EXECUTION;
          v = instance.scope[info.source];
        } else if (content !== NIL) {
          v = evaluate(sys, instance, content);
          if (v === NONE) return ERROR.EXECUTION;
        } else {
          v = {};
          const names = u32(capture + C.namelist);
          for (let i = 0; i < u32(capture + C.namelist_count); i++) {
            const info = expressionInfo(chart, u32(names + 4 * i));
            if (!info) return ERROR.EXECUTION;
            if (!Object.keys(instance.scope).includes(info.source)) {
              failure(sys, instance.serial, new Error('namelist: not a declared location'));
              return ERROR.EXECUTION;
            }
            v[info.source] = instance.scope[info.source];
          }
          const params = u32(capture + C.params);
          for (let i = 0; i < u32(capture + C.param_count); i++) {
            const value = evaluate(sys, instance, u32(params + 8 * i + 4));
            if (value === NONE) return ERROR.EXECUTION;
            v[stringOfImage(x.ta_image_string, chart.image, u32(params + 8 * i))] = value;
          }
        }
        setU64(out, payloadNew(sys, v));
        return OK;
      });
    },
    retain: (d, p) => retain(of(d), p),
    release: (d, p) => releasePayload(of(d), p),
    serialize(d, handle, buffer, capacity, length) {
      const sys = of(d);
      const instance = instanceAt(sys, handle);
      setU32(length, 0);
      if (!instance) return ERROR.INVALID;
      const s = scopeWire(sys, instance.scope);
      return s === null ? ERROR.UNSUPPORTED : writeOut(s, buffer, capacity, length);
    },
    deserialize(d, context, sv, buffer, length, out) {
      const sys = of(d);
      const state = valueOfWire(text(buffer, length), sys.callbacks.ref);
      if (!state || typeof state !== 'object') return ERROR.INVALID;
      const scope = svScope(sys, sv);
      if (state.v && typeof state.v === 'object') Object.assign(scope, state.v);
      if (state.e && typeof state.e === 'object') setEvent(scope, state.e);
      const [, handle] = instanceNew(sys, sys.charts.get(context), serialOf(view(sv + SV.sessionid)), scope);
      setU64(out, handle);
      return OK;
    },
    /* the previous bindings are the old variables; those the new chart does not declare go */
    migrate(d, handle, outside, bindings) {
      return withInstance(d, handle, outside, (sys, instance) => {
        const old = instance.scope;
        const scope = scopeNew(sys, old._sessionid, old._name, old._ioprocessors);
        const previous = { ...old };
        const { image, chart } = instance.chart;
        const declared = new Set();
        for (let i = 0, n = x.ta_image_data_count(image, chart); i < n; i++) {
          const data = dataInfo(image, chart, i);
          if (data) declared.add(data.id);
        }
        for (const k of Object.keys(old)) if (declared.has(k)) scope[k] = old[k];
        instance.scope = scope;
        setU64(bindings, payloadNew(sys, previous));
        return OK;
      });
    },
    to_wire(d, p, buffer, capacity, length) {
      let s;
      try {
        s = wire(payloadGet(of(d), p));
      } catch {
        return ERROR.INVALID;
      }
      return writeOut(s, buffer, capacity, length);
    },
    from_wire(d, json, out) {
      const sys = of(d);
      const v = valueOfWire(view(json), sys.callbacks.ref);
      if (v === undefined) return ERROR.INVALID;
      setU64(out, payloadNew(sys, v));
      return OK;
    },
    payload_serialize(d, p, buffer, capacity, length) {
      const status = NATIVE.to_wire(d, p, buffer, capacity, length);
      return status === ERROR.INVALID ? ERROR.UNSUPPORTED : status;
    },
    payload_deserialize(d, buffer, length, out) {
      const sys = of(d);
      const v = valueOfWire(text(buffer, length), sys.callbacks.ref);
      if (v === undefined) return ERROR.INVALID;
      setU64(out, payloadNew(sys, v));
      return OK;
    },
  };
  function withInstance(d, handle, outside, f) {
    const sys = of(d);
    const instance = instanceAt(sys, handle);
    if (!instance) return ERROR.EXECUTION;
    instance.outside = outside;
    try {
      return f(sys, instance);
    } finally {
      instance.outside = 0;
    }
  }

  /* the vtable structs, built once: entries by field, in TYPES order */
  const VTABLE = TYPES.ta_datamodel_vtable;
  const vtableFields = Object.entries(VTABLE.fields).filter(([, [, type]]) => type.includes('(*)'));
  const nameString = (s) => {
    const bytes = encoder.encode(`${s}\0`);
    const p = malloc(bytes.length, 1);
    m.u8().set(bytes, p);
    return p;
  };
  const nativeVtable = (() => {
    const entries = vtableFields.filter(([name]) => NATIVE[name]);
    const indices = tableFunctions(entries.map(([name, [, type]]) => ({ signature: signatureOf(type), fn: NATIVE[name] })));
    const p = malloc(VTABLE.size, 8);
    m.u8().fill(0, p, p + VTABLE.size);
    setU32(p + VTABLE.fields.size[0], VTABLE.size);
    setU32(p + VTABLE.fields.name[0], nameString('ecmascript'));
    entries.forEach(([name], i) => setU32(p + VTABLE.fields[name][0], indices[i]));
    return p;
  })();

  /* ---- the quickjs datamodel: its own vtable, with the entries this layer
   * needs to see (sessions created, destroyed, bound to events; charts) in
   * front of the engine's own, through the table */
  const engineVtable = x.ta_datamodel_ecmascript_vtable();
  const engine = (name) => table.get(u32(engineVtable + VTABLE.fields[name][0]));
  const table = x.__indirect_function_table;
  const quickjsVariables = (sys, handle, image, chart) =>
    scoped(() => {
      const ids = [];
      for (let i = 0, n = x.ta_image_data_count(image, chart); i < n; i++) {
        const data = dataInfo(image, chart, i);
        if (data) ids.push(data.id);
      }
      const expression = `({${ids.map((id) => `${JSON.stringify(id)}:typeof ${id}==='undefined'?undefined:${id}`).join(',')}})`;
      const source = viewOf(expression);
      let capacity = 4096;
      for (let attempt = 0; attempt < 8; attempt++) {
        const buffer = scratch(capacity, 1);
        const length = x.ta_datamodel_ecmascript_eval_json(sys.quickjs, BigInt(handle), source, buffer, capacity) >>> 0;
        if (length === 0xffffffff) break;
        if (length < capacity) return valueOfWire(text(buffer, length)) || {};
        capacity = length + 1;
      }
      return {};
    });
  const QUICKJS = {
    chart_attach(d, chartPointer, context) {
      const status = engine('chart_attach')(d, chartPointer, context);
      if (status === OK) {
        const sys = of(d);
        sys.charts.set(u32(context), chartInfo(chartPointer));
      }
      return status;
    },
    chart_detach(d, context) {
      const sys = of(d);
      if (sys) sys.charts.delete(context);
      engine('chart_detach')(d, context);
    },
    create(d, context, sv, init, out) {
      const status = engine('create')(d, context, sv, init, out);
      if (status === OK) {
        const sys = of(d);
        const chart = sys.charts.get(context);
        const session = { serial: serialOf(view(sv + SV.sessionid)), image: chart.image, chart: chart.chart, stamp: 0 };
        sys.sessions.set(n64(out), session);
        markChanged(sys, session, session.serial);
      }
      return status;
    },
    destroy(d, handle) {
      const sys = of(d);
      const session = sys && sys.sessions.get(Number(handle));
      if (session) {
        if (!sys.keepQuiet) graveAdd(sys, session.serial, quickjsVariables(sys, handle, session.image, session.chart));
        sys.sessions.delete(Number(handle));
      }
      engine('destroy')(d, handle);
    },
    bind_event(d, handle, outside, ev) {
      const sys = of(d);
      const session = sys.sessions.get(Number(handle));
      if (session) markChanged(sys, session, session.serial);
      return engine('bind_event')(d, handle, outside, ev);
    },
    serialize(d, handle, buffer, capacity, length) {
      const status = engine('serialize')(d, handle, buffer, capacity, length);
      if (status === ERROR.UNSUPPORTED) {
        const sys = of(d);
        sys.error = scoped(() => {
          const p = x.ta_datamodel_ecmascript_last_error(sys.quickjs);
          let n = 0;
          while (m.u8()[p + n]) n++;
          return text(p, n);
        });
      }
      return status;
    },
    deserialize(d, context, sv, buffer, length, out) {
      const status = engine('deserialize')(d, context, sv, buffer, length, out);
      if (status === OK) {
        const sys = of(d);
        const chart = sys.charts.get(context);
        sys.sessions.set(n64(out), { serial: serialOf(view(sv + SV.sessionid)), image: chart.image, chart: chart.chart, stamp: 0 });
      }
      return status;
    },
  };
  const quickjsVtable = (() => {
    const entries = vtableFields.filter(([name]) => QUICKJS[name]);
    const indices = tableFunctions(entries.map(([name, [, type]]) => ({ signature: signatureOf(type), fn: QUICKJS[name] })));
    const p = malloc(VTABLE.size, 8);
    m.u8().copyWithin(p, engineVtable, engineVtable + VTABLE.size);
    entries.forEach(([name], i) => setU32(p + VTABLE.fields[name][0], indices[i]));
    return p;
  })();

  /* ---- values between JavaScript and the system's datamodel */
  /* a payload as a JS value; undefined for none */
  const valueOfPayload = (sys, p, datamodel) => {
    if (!p || datamodel === NIL) return undefined;
    if (sys.native) return payloadGet(sys, p);
    const s = wireOfPayload(sys, p, datamodel);
    return s === undefined ? undefined : valueOfWire(s, sys.callbacks.ref);
  };
  /* a payload as neutral JSON; undefined for none or not neutral */
  const wireOfPayload = (sys, p, datamodel) => {
    if (!p || datamodel === NIL) return undefined;
    if (sys.native) {
      try {
        return wire(payloadGet(sys, p));
      } catch {
        return undefined;
      }
    }
    return scoped(() => {
      let capacity = 4096;
      for (let attempt = 0; attempt < 4; attempt++) {
        const buffer = scratch(capacity, 1);
        const length = out32();
        const status = engine('to_wire')(sys.quickjs, BigInt(p), buffer, capacity, length);
        if (status === OK) return text(buffer, u32(length));
        if (status !== ERROR.NO_MEMORY) return undefined;
        capacity = u32(length) + 1;
      }
      return undefined;
    });
  };
  /* a JS value as a new payload (one reference); 0 for undefined */
  const payloadOfValue = (sys, v) => {
    if (v === undefined) return 0;
    if (sys.native) return payloadNew(sys, v);
    let s;
    try {
      s = wire(v);
    } catch {
      throw new TypeError('event data must be a neutral value (docs/protocol.md)');
    }
    return payloadOfWire(sys, s, true);
  };
  const payloadOfWire = (sys, json, strict) => {
    if (sys.native) {
      const v = valueOfWire(json, sys.callbacks.ref);
      return v === undefined ? 0 : payloadNew(sys, v);
    }
    return scoped(() => {
      const outp = out64();
      if (engine('from_wire')(sys.quickjs, viewOf(json), outp) !== OK) {
        if (strict) throw new TypeError('event data must be a neutral value (docs/protocol.md)');
        return 0;
      }
      return n64(outp);
    });
  };
  const payloadDrop = (sys, p) => {
    if (!p) return;
    if (sys.native) releasePayload(sys, p);
    else engine('release')(sys.quickjs, BigInt(p));
  };

  /* ---- system handles (bindings/node/src/system.c) */
  /* The system of a call: refused once it stopped, and from inside a run (an
   * expression calling into its system; later() queues such calls). From an
   * effect handler (running 2) or a synchronous send (running 3) inputs go
   * in and the pump that is running goes on. */
  const systemOf = (handle) => {
    const sys = handle && handle.__system;
    if (!sys) throw new TypeError('expected a system handle');
    if (!sys.host || sys.stopping) throw new Error('the system was stopped');
    if (sys.running === 1) throw new Error('the system is running (an expression cannot call into its system)');
    return sys;
  };
  const session = (v) => {
    const d = Number(v);
    return d > 0 ? BigInt(Math.trunc(d)) : 0n;
  };
  const decimal = (v) => BigInt(String(v).replace(/[^0-9].*$/, '') || 0);
  const ticks = (ms) => BigInt(Math.max(0, Math.trunc(Number(ms) || 0)));
  const NEVER = 0xffffffffffffffffn;

  /* config: { systemID, ecmascript: 'native' | 'quickjs', ioProcessors: [{ type, location, sync }],
   * maxActors, maxEvents, maxTimers, stringBytes, seed: [high, low], maxOps, memoryLimit, now } */
  function systemNew(callbacks, options) {
    const id = typeof options.systemID === 'string' ? options.systemID : '';
    if (!scoped(() => x.ta_address_system_valid(viewOf(id)))) throw new TypeError('createSystem: systemID is not a valid system id (docs/protocol.md)');
    const key = nextKey++;
    const start = typeof options.now === 'number' && options.now > 0 ? options.now : 0; /* a restored system's time */
    const sys = {
      key,
      id,
      host: 0,
      callbacks,
      native: options.ecmascript !== 'quickjs',
      quickjs: 0,
      running: 0,
      more: false,
      stopping: false,
      armed: -3,
      timeBase: performance.now() - start,
      later: [],
      keepQuiet: false,
      pump: 0,
      changed: [],
      graves: [],
      failures: [],
      effectList: null,
      syncList: [],
      instances: [],
      freeInstance: 0,
      liveInstances: 0,
      payloads: [],
      payloadRefs: [],
      payloadNext: [],
      freePayload: 0,
      livePayloads: 0,
      charts: new Map(),
      nextChart: 1,
      sessions: new Map(),
      images: [],
      io: [],
      invocations: [],
      parked: new Map(),
      wakeHeld: [],
      jobs: new Map(),
      links: 0,
      exporter: 0,
      exportSignals: 0,
      exportFlush: 0,
      otlp: null,
      otlpFlags: 0,
      watchValues: false,
      traceSink: false,
      traces: [],
      logs: [],
      runs: { buckets: new Array(RUN_BOUNDS.length + 1).fill(0), count: 0, sum: 0, min: 0, max: 0 },
      error: '',
      evaluating: null,
      blocks: [],
    };
    sys.inFunction = (stateID) => {
      const instance = sys.evaluating;
      if (!instance || !instance.outside) return false;
      return scoped(() => {
        const state = x.ta_image_state_by_name(instance.chart.image, instance.chart.chart, viewOf(String(stateID))) >>> 0;
        return state !== NIL && !!x.ta_datamodel_environment_in(instance.outside, state);
      });
    };
    /* the datamodel binding: the vtable and the pointer every entry gets */
    let datamodel;
    if (sys.native) datamodel = key;
    else {
      sys.quickjs = scoped(() => {
        const config = zeroed(size('ta_datamodel_ecmascript_config'));
        setU32(config, size('ta_datamodel_ecmascript_config'));
        setU32(config + at('ta_datamodel_ecmascript_config', 'max_ops'), typeof options.maxOps === 'number' ? options.maxOps >>> 0 : 0);
        setU32(config + at('ta_datamodel_ecmascript_config', 'memory_limit'), typeof options.memoryLimit === 'number' ? options.memoryLimit >>> 0 : 0);
        return x.ta_datamodel_ecmascript_new(config);
      });
      if (!sys.quickjs) throw new Error('cannot create the quickjs datamodel');
      datamodel = sys.quickjs;
    }
    sys.datamodel = datamodel;
    live.set(datamodel, sys);
    const binding = malloc(size('ta_datamodel_binding'), 8);
    setU32(binding, sys.native ? nativeVtable : quickjsVtable);
    setU32(binding + 4, datamodel);
    sys.blocks.push(binding);
    sys.binding = binding;
    /* strings that live as long as the system */
    const kept = (s) => {
      const bytes = encoder.encode(s);
      const q = malloc(bytes.length || 1, 1);
      bytesAt().set(bytes, q);
      sys.blocks.push(q);
      return [q, bytes.length];
    };
    /* I/O processors: descriptors whose strings live as long as the system */
    const io = (Array.isArray(options.ioProcessors) ? options.ioProcessors : []).slice(0, IO_PROCESSORS_MAX);
    const descriptors = malloc(size('ta_io_processor_descriptor') * (io.length || 1), 8);
    sys.blocks.push(descriptors);
    io.forEach((entry, i) => {
      const p = descriptors + size('ta_io_processor_descriptor') * i;
      const type = typeof entry.type === 'string' ? entry.type : '';
      const location = typeof entry.location === 'string' ? entry.location : '';
      for (const [field, value] of [['type', type], ['location', location]]) {
        const [q, n] = kept(value);
        setU32(p + at('ta_io_processor_descriptor', field), q);
        setU32(p + at('ta_io_processor_descriptor', field) + 4, n);
      }
      setU32(p + at('ta_io_processor_descriptor', 'flags'), entry.sync === true ? K.IO_PROCESSOR_SYNC : 0);
      setU32(p + at('ta_io_processor_descriptor', 'pad'), 0);
      sys.io.push({ sync: entry.sync === true, type, location });
    });
    const [idPointer, idLength] = kept(id);
    sys.idView = malloc(8, 4); /* the system id as a ta_string_view (the links' config) */
    sys.blocks.push(sys.idView);
    setU32(sys.idView, idPointer);
    setU32(sys.idView + 4, idLength);
    const config = malloc(size('ta_host_config'), 8);
    sys.blocks.push(config);
    bytesAt().fill(0, config, config + size('ta_host_config'));
    const H = offsets('ta_host_config');
    const S = H.system;
    const SC = offsets('ta_system_config');
    const count = (v, otherwise) => (typeof v === 'number' && v > 0 ? v >>> 0 : otherwise);
    setU32(config + H.size, size('ta_host_config'));
    setU32(config + H.system_id, idPointer);
    setU32(config + H.system_id + 4, idLength);
    setU32(config + S + SC.size, size('ta_system_config'));
    setU64(config + S + SC.ticks_per_second, 1000n); /* ms */
    setU32(config + S + SC.trace_rules, 64); /* watches add rules (docs/adr/0007) */
    setU32(config + S + SC.max_actors, count(options.maxActors, 64));
    setU32(config + S + SC.max_events, count(options.maxEvents, 256));
    setU32(config + S + SC.max_timers, count(options.maxTimers, 64));
    setU32(config + S + SC.string_bytes, count(options.stringBytes, 32768));
    if (Array.isArray(options.seed) && options.seed.length === 2) setU64(config + S + SC.trace_seed, (BigInt(options.seed[0] >>> 0) << 32n) | BigInt(options.seed[1] >>> 0));
    setU32(config + H.datamodels, binding);
    setU32(config + H.datamodel_count, 1);
    setU32(config + H.io_processors, io.length ? descriptors : 0);
    setU32(config + H.io_processor_count, io.length);
    x.ta_runtime_malloc(config + H.allocator);
    sys.config = config;
    return sys;
  }

  /* Free the host and everything of the system now: callbacks can close over
   * the JS owner of the handle. Migrations end with nothing moved. */
  function systemDestroy(sys) {
    if (!sys.host && !sys.binding) return;
    sys.keepQuiet = true;
    for (const resolve of sys.jobs.values()) resolve({ migrated: 0, failed: 0, pending: [] });
    sys.jobs.clear();
    if (sys.links) x.ta_links_free(sys.links);
    sys.links = 0;
    if (sys.exporter) x.ta_otlp_batch_free(sys.exporter);
    sys.exporter = 0;
    if (sys.host) x.ta_host_free(sys.host);
    sys.host = 0;
    if (sys.quickjs) x.ta_datamodel_ecmascript_free(sys.quickjs);
    sys.quickjs = 0;
    for (const instance of sys.instances) if (instance && instance.live) free(instance.string);
    for (const p of sys.blocks) free(p);
    sys.blocks = [];
    if (sys.otlp) free(sys.otlp.memory);
    sys.otlp = null;
    live.delete(sys.datamodel);
    sys.binding = 0;
    sys.instances = [];
    sys.payloads = [];
    sys.charts.clear();
    sys.sessions.clear();
    sys.invocations = [];
    sys.parked.clear();
    sys.later = [];
    sys.callbacks = null;
  }
  const handleOf = (sys) => ({ __system: sys });

  /* The system's time in ms: the caller's clock (callbacks.now), else the monotonic one. */
  const systemNow = (sys) => {
    if (sys.callbacks.now) {
      const ms = sys.callbacks.now();
      return typeof ms === 'number' && ms > 0 ? ms : 0;
    }
    return performance.now() - sys.timeBase;
  };
  const tickNow = (sys) => ticks(systemNow(sys));

  /* later(system, fn) -> true when called from inside a run (an expression
   * calling into its system): fn is called between two runs, in order with the
   * others; false otherwise (the caller goes on at once). */
  const later = (handle, fn) => {
    const sys = handle && handle.__system;
    if (!sys || sys.running !== 1 || !sys.host || sys.stopping) return false;
    sys.later.push(fn);
    return true;
  };
  /* the calls kept during the last run, in order; the first one thrown is returned */
  const callLater = (sys) => {
    let thrown = null;
    for (const f of sys.later.splice(0)) {
      try {
        f();
      } catch (error) {
        if (!thrown) thrown = { error };
      }
    }
    return thrown;
  };

  /* ---- tracing (src/trace.c) */
  const traceWall = (sys) => {
    const ms = sys.callbacks.clock ? Number(sys.callbacks.clock()) : 0;
    return ms > 0 ? BigInt(Math.trunc(ms * 1e6)) : 0n;
  };
  const traceFuel = (sys) =>
    scoped(() => {
      const stats = zeroed(size('ta_host_stats'));
      setU32(stats, size('ta_host_stats'));
      x.ta_host_stats_get(sys.host, stats);
      return u64(stats + at('ta_host_stats', 'fuel_clock'));
    });
  /* the staged requests, onto this turn's */
  const tracePut = (sys) =>
    scoped(() => {
      for (const [sizeOf, put, list] of [[x.ta_otlp_traces_size, x.ta_otlp_put_traces, sys.traces], [x.ta_otlp_logs_size, x.ta_otlp_put_logs, sys.logs]]) {
        const n = sizeOf(sys.otlp.otlp);
        if (!n) continue;
        const p = scratch(n, 8);
        list.push(copyOut(p, put(sys.otlp.otlp, p, n)));
      }
    });
  const traceGrow = (sys) => {
    const stat = otlpStats(sys.otlp.otlp);
    const moved = otlpRelocate(sys.otlp, { open_spans: stat.open_capacity * 2, chunks: stat.chunks_capacity * 2, staging_bytes: stat.staging_capacity, log_bytes: stat.log_capacity });
    if (!moved) return false;
    sys.otlp = moved;
    return true;
  };
  /* every segment's records into the encoder (or let go without one) */
  const traceDrain = (sys) =>
    scoped(() => {
      const view_ = zeroed(size('ta_trace_view'));
      const segmentInfo = zeroed(size('ta_host_segment_info'));
      const input_ = zeroed(size('ta_otlp_input'));
      const cursor = out32();
      const I = offsets('ta_otlp_input');
      for (let k = 0, n = x.ta_host_segment_count(sys.host); k < n; k++) {
        const system = x.ta_host_system(sys.host, k);
        if (!system) continue;
        setU32(segmentInfo, size('ta_host_segment_info'));
        x.ta_host_segment_info_get(sys.host, k, segmentInfo);
        while (x.ta_system_trace(system, view_)) {
          if (sys.otlp) {
            setU32(input_, size('ta_otlp_input'));
            setU32(input_ + I.source, k);
            bytesAt().copyWithin(input_ + I.view, view_, view_ + size('ta_trace_view'));
            setU32(input_ + I.image, u32(segmentInfo + at('ta_host_segment_info', 'image')));
            setU32(input_ + I.datamodels, sys.binding);
            setU32(input_ + I.datamodel_count, 1);
            setU32(cursor, 0);
            let status;
            while ((status = x.ta_otlp_read(sys.otlp.otlp, input_, cursor)) !== K.OTLP_READ) {
              if (status === K.OTLP_FLUSH) tracePut(sys);
              else if (!traceGrow(sys)) break; /* no memory: these records are lost */
            }
          }
          x.ta_system_trace_ack(system, u32(view_ + at('ta_trace_view', 'length')));
        }
      }
    });
  const concat = (list) => {
    if (!list.length) return undefined;
    if (list.length === 1) return list[0];
    const out = new Uint8Array(list.reduce((n, b) => n + b.length, 0));
    let o = 0;
    for (const b of list) {
      out.set(b, o);
      o += b.length;
    }
    return out;
  };

  /* ---- the trace options of the API, as the host takes them (ta_host_trace_options) */
  const SAMPLE_WHOLE = 1000000;
  const WATCH_MS = 15 * 60 * 1000; /* a watch ends by itself after this, unless told */
  const RULE_AT = [['start', K.TRACE_AT_START], ['take', K.TRACE_AT_TAKE]];
  const RULE_FROM = [['host', K.TRACE_FROM_HOST], ['actor', K.TRACE_FROM_ACTOR], ['timer', K.TRACE_FROM_TIMER], ['spawn', K.TRACE_FROM_SPAWN], ['sampled', K.TRACE_FROM_SAMPLED], ['unsampled', K.TRACE_FROM_UNSAMPLED]];
  /* names (one, or an array of them) as bits of `table` */
  const bitsOf = (v, table, what) => {
    let bits = 0;
    if (v === undefined) return bits;
    for (const name of Array.isArray(v) ? v : [v]) {
      const entry = typeof name === 'string' && table.find(([n]) => n === name);
      if (!entry) throw new TypeError(`${what}: one of ${table.slice(0, 3).map(([n]) => n).join(', ')}${table.length > 3 ? ', ...' : ''}`);
      bits |= entry[1];
    }
    return bits >>> 0;
  };
  /* an array of kind names as a mask (ta_trace_kind_by_name) */
  const kindsMask = (names) => {
    let mask = 0;
    for (const name of names) {
      const kind = typeof name === 'string' ? scoped(() => x.ta_trace_kind_by_name(viewOf(name))) : 0;
      if (!kind) throw new TypeError('not a trace kind (see TRACE_KINDS)');
      mask |= 1 << kind;
    }
    return mask >>> 0;
  };
  const DETAIL_ANY = 'detail: "summary", "decisions", "full", "off" or { kinds, values }';
  /* a detail's kinds: "summary" | "decisions" | "full" | "off" | { kinds, values }; full
   * when undefined; `top`: the system's own detail, where "off" is no detail */
  const detailKinds = (detail, kinds, values, top) => {
    let out;
    if (Array.isArray(kinds)) out = kindsMask(kinds);
    else if (detail === undefined) out = K.TRACE_FULL;
    else if (typeof detail === 'string') {
      out = scoped(() => {
        const o = out32();
        return x.ta_trace_detail_by_name(viewOf(detail), o) === OK ? u32(o) : null;
      });
      if (out === null || (top && detail === 'off')) throw new TypeError(top ? 'detail: "summary", "decisions" or "full"' : 'detail: "summary", "decisions", "full" or "off"');
    } else if (detail && typeof detail === 'object') {
      if (!Array.isArray(detail.kinds)) throw new TypeError(DETAIL_ANY);
      out = kindsMask(detail.kinds);
      values = values || detail.values === true;
    } else throw new TypeError(DETAIL_ANY);
    return (values ? out | K.TRACE_VALUES : out) >>> 0;
  };
  /* a ratio from 0 to 1 as parts of SAMPLE_WHOLE (all of them when undefined) */
  const sampleParts = (v) => {
    if (v === undefined) return SAMPLE_WHOLE;
    const ratio = typeof v === 'number' ? v : -1;
    if (!(ratio >= 0 && ratio <= 1)) throw new RangeError('sample: a ratio from 0 to 1');
    return Math.trunc(ratio * SAMPLE_WHOLE + 0.5);
  };
  const numberOr0 = (v) => (typeof v === 'number' ? v : 0);
  /* a rule of the API ({ at, from, chart, actor, event, session, sample, detail, values,
   * perSecond, burst }) as the fields of a ta_trace_rule */
  const ruleOf = (rule) => {
    if (!rule || typeof rule !== 'object') throw new TypeError('a trace rule is an object');
    const r = { sample: 0, parts: 0, whole: 0, session: 0n, detail: NIL };
    if (typeof rule.sample === 'boolean') r.sample = rule.sample ? K.TRACE_SAMPLE : K.TRACE_DROP;
    else if (rule.sample !== undefined) {
      r.sample = K.TRACE_RATIO;
      r.whole = SAMPLE_WHOLE;
      r.parts = sampleParts(rule.sample);
    }
    for (const field of ['chart', 'actor', 'event']) r[field] = typeof rule[field] === 'string' ? rule[field] : '';
    if (rule.session !== undefined) r.session = decimal(rule.session);
    r.at = bitsOf(rule.at, RULE_AT, 'at');
    r.from = bitsOf(rule.from, RULE_FROM, 'from');
    if (rule.detail !== undefined) r.detail = detailKinds(rule.detail, undefined, rule.values === true, false);
    r.perSecond = numberOr0(rule.perSecond) >>> 0;
    r.burst = numberOr0(rule.burst) >>> 0;
    return r;
  };
  /* rules as a ta_trace_rule array in scratch */
  const rulesIn = (rules) => {
    const R = offsets('ta_trace_rule');
    const array = zeroed(size('ta_trace_rule') * (rules.length || 1));
    rules.forEach((r, i) => {
      const p = array + size('ta_trace_rule') * i;
      setU32(p + R.size, size('ta_trace_rule'));
      setU32(p + R.at, r.at);
      for (const field of ['chart', 'actor', 'event']) putView(p + R[field], r[field]);
      setU64(p + R.session, r.session);
      setU32(p + R.from, r.from);
      setU32(p + R.sample, r.sample);
      setU32(p + R.sample_parts, r.parts);
      setU32(p + R.sample_whole, r.whole);
      setU32(p + R.detail, r.detail);
      setU32(p + R.per_second, r.perSecond);
      setU32(p + R.burst, r.burst);
    });
    return array;
  };
  const traceStatusError = (status) =>
    new Error(status === ERROR.NO_MEMORY ? 'traces: more rules than a system holds (64), more than 6 different details in rules and watches, or no memory' : 'traces: a rule is malformed, or names more than 16 actors');
  /* the encoder's flags with what the watches need (their values) */
  const encoderFlags = (sys) => {
    if (sys.otlp) x.ta_otlp_set_flags(sys.otlp.otlp, (sys.otlpFlags | (sys.watchValues ? K.OTLP_VALUES : 0)) >>> 0);
  };

  /* ---- external invocations (src/invoke.c) */
  const invocationAt = (sys, segment, token) => sys.invocations.find((inv) => inv.segment === segment && inv.token === token);
  const invocationDrop = (sys, inv) => {
    const i = sys.invocations.indexOf(inv);
    if (i >= 0) {
      sys.invocations[i] = sys.invocations[sys.invocations.length - 1];
      sys.invocations.pop();
    }
  };
  /* TA_EFFECT_INVOKE: a new invocation { session, segment, token, invokeid }, kept */
  const invocationNew = (sys, e) => {
    const object = { session: n64(e + EFFECT.session), segment: u32(e + EFFECT.segment), token: u32(e + EFFECT.token), invokeid: view(e + EFFECT.sendid) };
    sys.invocations.push({ segment: object.segment, token: object.token, session: object.session, invokeid: object.invokeid, object });
    return object;
  };
  /* the invocation a TA_EFFECT_SEND is for (an invoked service, or "#_<invokeid>"), if any */
  const invocationOfSend = (sys, e, target) => {
    let inv;
    const token = u32(e + EFFECT.token);
    if (u32(e + EFFECT.io_processor) === NIL && token) inv = invocationAt(sys, u32(e + EFFECT.segment), token);
    if (!inv && target.length > 2 && target.startsWith('#_')) {
      const session = n64(e + EFFECT.session);
      inv = sys.invocations.find((c) => c.session === session && c.invokeid === target.slice(2));
    }
    return inv ? inv.object : undefined;
  };

  /* ---- hibernated actors in flight: a blob until the store has it, a wake asked (src/persist.c) */
  const parkedAdd = (sys, session) => {
    let p = sys.parked.get(session);
    if (!p) sys.parked.set(session, (p = { blob: null, waking: false, woken: false }));
    return p;
  };
  /* A TA_EFFECT_WAKE: true when the caller is to load the blob (once per actor);
   * a blob still held is woken from after the run's effects (wakeHeld) */
  const wakeEffect = (sys, session) => {
    const p = sys.parked.get(session);
    if (p && p.blob) {
      sys.wakeHeld.push(session);
      return false;
    }
    if (p && p.waking) return false;
    parkedAdd(sys, session).waking = true;
    return true;
  };
  /* ta_host_wake with no calls in meanwhile (charts attach, blobs are read) */
  const wakeFrom = (sys, sessionID, bytes) => {
    const was = sys.running;
    sys.running = 1;
    try {
      return scoped(() => x.ta_host_wake(sys.host, BigInt(sessionID), bytesIn(bytes), bytes.length));
    } finally {
      sys.running = was;
    }
  };
  const wakeHeld = (sys) => {
    sys.more = true; /* their work runs in this pump */
    for (const s of sys.wakeHeld.splice(0)) {
      const p = sys.parked.get(s);
      if (!p) continue;
      if (p.blob) wakeFrom(sys, s, p.blob);
      p.blob = null; /* stored() says the store's copy is stale */
      p.woken = true;
    }
  };

  /* ---- links (src/link.c) */
  const LINK_OPERATIONS = ['', 'send', 'close', 'open', 'hello'];
  /* the system's links, made with the first one */
  const linksOf = (sys) => {
    if (sys.links) return sys.links;
    sys.links = scoped(() => {
      const L = offsets('ta_links_config');
      const config = zeroed(size('ta_links_config'));
      setU32(config + L.size, size('ta_links_config'));
      setU32(config + L.host, sys.host);
      bytesAt().copyWithin(config + L.system_id, sys.idView, sys.idView + 8);
      setU32(config + L.datamodels, sys.binding);
      setU32(config + L.datamodel_count, 1);
      x.ta_runtime_malloc(config + L.allocator);
      const out = out32();
      return x.ta_links_new(config, out) === OK ? u32(out) : 0;
    });
    if (!sys.links) throw new Error('links: no memory');
    return sys.links;
  };
  /* what the links asked of the transports, as effects { kind: 'link', op, connection, text } */
  const linksOutput = (sys) => {
    if (!sys.links) return;
    scoped(() => {
      const out = out32();
      const n = x.ta_links_output(sys.links, out);
      const O = offsets('ta_link_output');
      for (let i = 0; i < n; i++) {
        const p = u32(out) + size('ta_link_output') * i;
        const kind = u32(p + O.kind);
        const e = { kind: 'link', op: LINK_OPERATIONS[kind] || '', connection: u32(p + O.connection) };
        if (viewLength(p + O.text)) e.text = view(p + O.text);
        sys.effectList.push(e);
      }
      x.ta_links_output_ack(sys.links);
    });
  };
  /* the router's view of a peer (ta_host_peer_status), or null when it is gone */
  const peerStatus = (sys, peer) =>
    scoped(() => {
      const P = offsets('ta_peer_status');
      const s = zeroed(size('ta_peer_status'));
      setU32(s, size('ta_peer_status'));
      if (!peer || x.ta_host_peer_status(sys.host, peer, s) !== OK) return null;
      return {
        id: view(s + P.id),
        state: ['connecting', 'open', 'reconnecting', 'closed'][u32(s + P.state) & 3],
        queued: u32(s + P.queued),
        rtt: u32(s + P.round_trip_time),
        lag: n64(s + P.lag),
        sent: n64(s + P.sent),
        received: n64(s + P.received),
        errors: n64(s + P.errors),
        duplicates: n64(s + P.duplicates),
      };
    });
  /* string views of an array of strings, in scratch */
  const viewsIn = (list) => {
    const p = scratch(8 * (list.length || 1), 4);
    list.forEach((s, i) => putView(p + 8 * i, typeof s === 'string' ? s : ''));
    return p;
  };
  /* { trusted, judge, announce (true or ids), maxQueue, queueTTLMs, resumeMs, pingMs,
   * maxFrame, name, credential (JSON text), exports: [[name, events], ...] } */
  const linkOpen = (handle, o, server) => {
    const sys = systemOf(handle);
    const links = linksOf(sys);
    const status = scoped(() => {
      const L = offsets('ta_link_options');
      const p = zeroed(size('ta_link_options'));
      setU32(p + L.size, size('ta_link_options'));
      let flags = 0;
      if (o.trusted === true) flags |= K.LINK_TRUSTED;
      if (o.judge === true) flags |= K.LINK_JUDGE;
      if (Array.isArray(o.announce) || o.announce === true) flags |= K.LINK_ANNOUNCE;
      setU32(p + L.flags, flags);
      setU32(p + L.max_queue, numberOr0(o.maxQueue) >>> 0);
      setU64(p + L.queue_ttl, ticks(o.queueTTLMs));
      setU64(p + L.resume, ticks(o.resumeMs));
      setU64(p + L.ping_interval, ticks(o.pingMs));
      setU32(p + L.max_frame, numberOr0(o.maxFrame) >>> 0);
      putView(p + L.name, typeof o.name === 'string' ? o.name : '');
      putView(p + L.credential, typeof o.credential === 'string' ? o.credential : '');
      if (Array.isArray(o.announce)) {
        setU32(p + L.announce, viewsIn(o.announce));
        setU32(p + L.announce_count, o.announce.length);
      }
      const exports = Array.isArray(o.exports) ? o.exports : [];
      setU32(p + L.exports, viewsIn(exports.map((pair) => pair && pair[0])));
      setU32(p + L.export_events, viewsIn(exports.map((pair) => pair && pair[1])));
      setU32(p + L.export_count, exports.length);
      const out = out32();
      const s = server ? x.ta_links_serve(links, p, out) : x.ta_links_connect(links, p, tickNow(sys), out);
      return s === OK ? { handle: u32(out) } : { status: s };
    });
    if (status.status !== undefined) throw new Error(status.status === ERROR.INVALID ? 'createLink: name is not a system id' : 'links: cannot add the peer');
    return status.handle; /* the next pump asks for a link's transport */
  };
  /* the system and its links of a call (system, handle, ...) */
  const linksCall = (handle) => {
    const sys = systemOf(handle);
    linksOf(sys);
    return sys;
  };

  /* ---- the exporter (src/export.c) */
  const SIGNALS = ['traces', 'logs', 'metrics'];
  /* bytes of a signal for the exporter, when it posts that signal */
  const exporterAdd = (sys, signal, bytes) => {
    if (sys.exporter && bytes && bytes.length && sys.exportSignals & (1 << signal)) scoped(() => x.ta_otlp_batch_add(sys.exporter, signal, bytesIn(bytes), bytes.length, tickNow(sys)));
  };
  /* the bodies of ta_otlp_batch_next (mode: 0, flush, drain) as { signal, body } */
  const exporterNext = (sys, mode, each) => {
    if (!sys.exporter) return;
    scoped(() => {
      const signal = out32();
      const body = zeroed(8, 4);
      const now = tickNow(sys);
      while (x.ta_otlp_batch_next(sys.exporter, now, mode, signal, body)) each({ signal: SIGNALS[u32(signal)], body: copyOut(u32(body), u32(body + 4)) });
    });
  };
  /* the bodies due (all of them when flushing), as effects { kind: 'post', signal, body } */
  const exporterOutput = (sys) => {
    exporterNext(sys, sys.exportFlush, (post) => sys.effectList.push({ kind: 'post', ...post }));
    sys.exportFlush = 0;
  };
  const exporterDeadline = (sys) => (sys.exporter ? x.ta_otlp_batch_deadline(sys.exporter) : NEVER);

  /* ---- metrics (src/metrics.c) */
  const RUN_BOUNDS = [0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25];
  const runRecord = (sys, seconds) => {
    const runs = sys.runs;
    let bucket = 0;
    while (bucket < RUN_BOUNDS.length && seconds > RUN_BOUNDS[bucket]) bucket++;
    runs.buckets[bucket]++;
    runs.min = runs.count && runs.min < seconds ? runs.min : seconds;
    runs.max = runs.count && runs.max > seconds ? runs.max : seconds;
    runs.count++;
    runs.sum += seconds;
  };
  /* the binding's own metrics: tinyactors.run.duration (how long each pump held the
   * thread) and, with an exporter, tinyactors.telemetry.bytes by outcome */
  const ownMetrics = (sys) => {
    const runs = sys.runs;
    const metrics = [{ name: 'tinyactors.run.duration', unit: 's', kind: 'histogram', points: [{ count: runs.count, sum: runs.sum, min: runs.min, max: runs.max, bounds: RUN_BOUNDS, buckets: runs.buckets }] }];
    if (sys.exporter) {
      const stats = exportStats(sys);
      metrics.push({ name: 'tinyactors.telemetry.bytes', unit: 'By', kind: 'sum', points: ['posted', 'failed', 'dropped'].map((outcome) => ({ attributes: { 'tinyactors.telemetry.outcome': outcome }, value: stats[outcome] })) });
    }
    return metrics;
  };
  const exportStats = (sys) =>
    scoped(() => {
      const B = offsets('ta_otlp_batch_stats');
      const stats = zeroed(size('ta_otlp_batch_stats'));
      setU32(stats, size('ta_otlp_batch_stats'));
      if (sys.exporter) x.ta_otlp_batch_stats_get(sys.exporter, stats);
      return { posted: n64(stats + B.posted), failed: n64(stats + B.failed), dropped: n64(stats + B.dropped), waiting: n64(stats + B.waiting), inFlight: u32(stats + B.in_flight) };
    });

  /* ---- the run loop (src/system.c) */
  const graveContext = (sys, serial) => {
    for (const [s, context] of sys.graves) if (s === serial) return context;
    return undefined;
  };
  /* TA_EFFECT_REQUEST: an <invoke src> that is not a chart of its image: the
   * chart of that source name in the segment's image, if there is one */
  const answerRequest = (sys, e) =>
    scoped(() => {
      const segment = u32(e + EFFECT.segment);
      const image = sys.images[segment];
      let chart = NIL;
      if (image) {
        const want = sourceName(view(e + EFFECT.auxiliary));
        const info = zeroed(size('ta_image_info'));
        setU32(info, size('ta_image_info'));
        x.ta_image_get_info(image.image, info);
        for (let c = 0; c < u32(info + at('ta_image_info', 'chart_count')) && chart === NIL; c++) if (chartSource(image.image, c) === want) chart = c;
        if (chart === NIL) chart = x.ta_image_chart_by_name(image.image, viewOf(want)) >>> 0;
      }
      x.ta_host_resume(sys.host, segment, u32(e + EFFECT.token), chart === NIL ? ERROR.INVALID : OK, chart === NIL ? 0n : BigInt(chart + 1));
    });
  /* TA_HOST_EFFECT_JOB_DONE: migrate()'s promise */
  const jobDone = (sys, e) => {
    const job = u32(e + EFFECT.job);
    const resolve = sys.jobs.get(job);
    if (!resolve) return;
    sys.jobs.delete(job);
    const pending = scoped(() => {
      const n = x.ta_host_job_pending(sys.host, job, 0, 0);
      const buffer = scratch(8 * (n || 1), 8);
      const k = x.ta_host_job_pending(sys.host, job, buffer, n);
      return Array.from({ length: k }, (_, i) => n64(buffer + 8 * i));
    });
    resolve({ migrated: u32(e + EFFECT.migrated), failed: u32(e + EFFECT.failed), pending });
  };
  const traceparentAt = (e) => traceparentOf(e + EFFECT.trace);
  function effectOut(sys, e) {
    const list = sys.effectList;
    switch (u32(e + EFFECT.kind)) {
      case K.EFFECT_LOG:
        list.push({ kind: 'log', session: n64(e + EFFECT.session), root: n64(e + EFFECT.root), label: view(e + EFFECT.auxiliary), text: view(e + EFFECT.event) });
        break;
      case K.EFFECT_DONE: {
        const session = n64(e + EFFECT.session);
        list.push({ kind: 'done', session, final: view(e + EFFECT.auxiliary), output: valueOfPayload(sys, n64(e + EFFECT.payload), u32(e + EFFECT.datamodel)), context: graveContext(sys, session) });
        break;
      }
      case K.EFFECT_ERROR: {
        const session = n64(e + EFFECT.session);
        list.push({ kind: 'error', session, code: u32(e + EFFECT.flags), context: graveContext(sys, session) });
        break;
      }
      case K.EFFECT_SEND: {
        const io = u32(e + EFFECT.io_processor);
        const target = view(e + EFFECT.target);
        const out = {
          kind: 'send',
          session: n64(e + EFFECT.session),
          root: n64(e + EFFECT.root),
          segment: u32(e + EFFECT.segment),
          token: u32(e + EFFECT.token),
          ioProcessor: io === NIL ? -1 : io,
          target,
          event: view(e + EFFECT.event),
          sendid: view(e + EFFECT.sendid),
          data: valueOfPayload(sys, n64(e + EFFECT.payload), u32(e + EFFECT.datamodel)),
          traceparent: traceparentAt(e),
        };
        const receiving = invocationOfSend(sys, e, target); /* an invoked service */
        if (receiving) out.invocation = receiving;
        /* a synchronous processor's send is answered before the next run (pump) */
        (io < sys.io.length && sys.io[io].sync ? sys.syncList : list).push(out);
        break;
      }
      case K.EFFECT_INVOKE:
        list.push({
          kind: 'invoke',
          session: n64(e + EFFECT.session),
          segment: u32(e + EFFECT.segment),
          token: u32(e + EFFECT.token),
          type: view(e + EFFECT.event),
          src: view(e + EFFECT.target),
          invokeid: view(e + EFFECT.sendid),
          params: valueOfPayload(sys, n64(e + EFFECT.payload), u32(e + EFFECT.datamodel)),
          traceparent: traceparentAt(e),
          invocation: invocationNew(sys, e),
        });
        break;
      case K.EFFECT_UNINVOKE: {
        /* an invocation core.mjs knows of */
        const inv = invocationAt(sys, u32(e + EFFECT.segment), u32(e + EFFECT.token));
        if (!inv) break;
        invocationDrop(sys, inv);
        list.push({ kind: 'uninvoke', invocation: inv.object });
        break;
      }
      case K.EFFECT_WAKE: {
        /* the caller loads the blob, unless it is still held */
        const session = n64(e + EFFECT.session);
        if (wakeEffect(sys, session)) list.push({ kind: 'wake', session });
        break;
      }
      case K.EFFECT_REQUEST:
        answerRequest(sys, e);
        break;
      case K.HOST_EFFECT_JOB_DONE:
        jobDone(sys, e);
        break;
      case K.HOST_EFFECT_SEGMENT_FREED: {
        const segment = u32(e + EFFECT.segment);
        sys.images[segment] = undefined;
        list.push({ kind: 'freed', segment });
        break;
      }
      default:
        break;
    }
  }

  /* One pass: ta_host_run in rounds until idle (or RUN_ROUNDS runs); every
   * effect, the changed sessions and the traces go to callbacks.effects at once.
   * Returns the run code; throws what callbacks.effects or callbacks.sync threw. */
  function pumpOnce(sys) {
    const now = tickNow(sys);
    x.ta_host_advance_time(sys.host, now);
    if (sys.links) x.ta_links_time(sys.links, now);
    sys.running = 1;
    sys.pump++;
    sys.effectList = [];
    sys.syncList = [];
    sys.changed = [];
    let thrown = null; /* by callbacks.sync: rethrown once the pump is done */
    let status = K.RUN_IDLE;
    let deadline = 0n;
    if (sys.quickjs) x.ta_datamodel_ecmascript_wasm_set_time(Number(traceWall(sys)) / 1e6); /* the module has no clock of its own */
    const m0 = mark();
    try {
      const run = scratch(size('ta_run'), 8);
      const effects = out32();
      for (let round = 0; round < RUN_ROUNDS; round++) {
        if (sys.otlp) x.ta_otlp_mark(sys.otlp.otlp, traceFuel(sys), traceWall(sys));
        x.ta_host_run(run, sys.host, RUN_BUDGET);
        status = u32(run + at('ta_run', 'status'));
        deadline = u64(run + at('ta_run', 'next_deadline'));
        if (sys.otlp) x.ta_otlp_mark(sys.otlp.otlp, traceFuel(sys), traceWall(sys));
        traceDrain(sys); /* before the effects: what they cause comes after */
        const n = x.ta_host_effects(sys.host, effects);
        const base = u32(effects);
        for (let i = 0; i < n; i++) {
          const e = base + EFFECT_SIZE * i;
          if (!sys.links || !x.ta_links_effect(sys.links, e, now)) effectOut(sys, e);
        }
        x.ta_host_effects_ack(sys.host);
        if (sys.links && x.ta_links_deadline(sys.links) <= now) {
          x.ta_links_time(sys.links, now); /* refused envelopes: errors back */
          sys.more = true;
        }
        if (sys.wakeHeld.length) wakeHeld(sys); /* their work runs in the next round */
        /* TA_RUN_EFFECTS: the senders wait; callbacks.sync answers each send now,
         * feeding the host (send, post, deliver) before the next run */
        const syncList = sys.syncList;
        sys.syncList = [];
        sys.running = 3;
        for (const e of syncList) {
          if (thrown) break;
          try {
            sys.callbacks.sync(e);
          } catch (error) {
            thrown = { error };
          }
        }
        if (sys.later.length && !thrown) thrown = callLater(sys); /* (running 3) */
        sys.running = 1;
        if (thrown || sys.stopping || status === K.RUN_IDLE || status === K.RUN_FAULT) break;
      }
    } catch (error) {
      sys.running = 0;
      throw error;
    } finally {
      release(m0);
    }
    const list = sys.effectList;
    for (const [serial, error] of sys.failures) list.push({ kind: 'exception', session: serial, error }); /* exceptions in expressions */
    sys.failures = [];
    sys.graves = [];
    const changed = sys.changed.length ? sys.changed : undefined;
    if (sys.otlp) tracePut(sys); /* the spans that ended in this call */
    const traceBytes = concat(sys.traces);
    const logBytes = concat(sys.logs);
    sys.traces = [];
    sys.logs = [];
    exporterAdd(sys, K.OTLP_TRACES, traceBytes);
    exporterAdd(sys, K.OTLP_LOGS, logBytes);
    const traces = sys.traceSink ? traceBytes : undefined;
    const logs = sys.traceSink ? logBytes : undefined;
    linksOutput(sys); /* what the transports are to do */
    exporterOutput(sys); /* and the exporter's posts */
    sys.effectList = null;
    sys.running = 0;
    if (list.length || changed || traces || logs) {
      sys.running = 2; /* inputs from the handlers go in: the pump goes on */
      try {
        sys.callbacks.effects(list.length ? list : undefined, changed, traces, logs);
      } finally {
        sys.running = 0;
      }
    }
    if (thrown) throw thrown.error;
    if (status !== K.RUN_IDLE) return -2;
    return deadline === NEVER ? -1 : Number(deadline);
  }

  /* Arm the caller's one timer for a run code (callbacks.schedule): 0 to run
   * soon, -1 for nothing to do, else the delay to the next deadline. A caller's
   * own clock (callbacks.now) arms nothing: time moves when it pumps. */
  const arm = (sys, code) => {
    if (sys.callbacks.now || !sys.callbacks.schedule || sys.stopping) return;
    if (code === sys.armed) return;
    sys.armed = code;
    const delay = code === -2 ? 0 : code < 0 ? -1 : code - systemNow(sys);
    sys.callbacks.schedule(delay < 0 && code >= 0 ? 0 : delay);
  };

  /* Run until idle: passes of RUN_ROUNDS runs, again while effect handlers
   * gave the host input (at most RUN_ROUNDS passes, then the timer goes on);
   * then the timer is armed. Returns the run code; throws what an effect
   * handler threw. Inside a pump it only notes more work. */
  function pump(sys) {
    if (sys.running) {
      sys.more = true;
      return -2;
    }
    const started = performance.now();
    let code = -2;
    let failed = null;
    try {
      for (let pass = 0; pass < RUN_ROUNDS; pass++) {
        sys.more = false;
        code = pumpOnce(sys);
        if (sys.stopping || !sys.more) break;
        code = -2;
      }
    } catch (error) {
      failed = { error };
    }
    runRecord(sys, (performance.now() - started) / 1000);
    if (sys.stopping) {
      systemDestroy(sys);
      if (failed) throw failed.error;
      return -1;
    }
    if (failed) throw failed.error;
    /* pings, reconnects, resumption windows; posts due */
    const linkDue = sys.links ? x.ta_links_deadline(sys.links) : NEVER;
    const postDue = exporterDeadline(sys);
    const due = linkDue < postDue ? linkDue : postDue;
    if (due !== NEVER && (code === -1 || (code >= 0 && Number(due) < code))) code = Number(due);
    arm(sys, sys.more ? -2 : code);
    return code;
  }

  /* After an input: from outside, the pump runs now; from an effect handler or
   * a synchronous send, the pump that is running goes on. */
  const inputDone = (sys, result) => {
    if (sys.running) sys.more = true;
    else pump(sys);
    return result;
  };
  /* a host call that runs JavaScript (charts attach, blobs are written): no calls in meanwhile */
  const entered = (sys, f) => {
    const was = sys.running;
    sys.running = 1;
    try {
      return f();
    } finally {
      sys.running = was;
    }
  };

  /* an event's fields at p (a ta_event_in) */
  const eventIn = (p, fields) => {
    bytesAt().fill(0, p, p + size('ta_event_in'));
    setU32(p + EVENT_IN.size, size('ta_event_in'));
    for (const field of ['name', 'sendid', 'origin', 'origintype', 'raw', 'traceparent']) if (typeof fields[field] === 'string') putView(p + EVENT_IN[field], fields[field]);
    setU32(p + EVENT_IN.flags, numberOr0(fields.flags) >>> 0);
    setU32(p + EVENT_IN.token, numberOr0(fields.token) >>> 0);
    setU64(p + EVENT_IN.data, BigInt(fields.data || 0));
    return p;
  };
  const optional = (s) => (typeof s === 'string' ? s : undefined);

  /* a blob through a call that sizes it first (length = needed on TA_ERROR_NO_MEMORY) */
  const writeBlob = (sys, writer) => {
    sys.error = '';
    sys.keepQuiet = true;
    try {
      return entered(sys, () =>
        scoped(() => {
          let capacity = 4096;
          const length = out32();
          for (let attempt = 0; attempt < 8; attempt++) {
            const buffer = scratch(capacity, 8);
            const status = writer(buffer, capacity, length);
            if (status === OK) return [OK, copyOut(buffer, u32(length))];
            if (status !== ERROR.NO_MEMORY) return [status];
            capacity = Math.max(u32(length), capacity * 2);
          }
          return [ERROR.NO_MEMORY];
        }),
      );
    } finally {
      sys.keepQuiet = false;
    }
  };
  const statusError = (what, status, detail) => {
    const why =
      status === ERROR.STATE
        ? 'the actor is not at a stable point (busy, invoking, or queued events)'
        : status === ERROR.UNSUPPORTED
          ? detail
          : status === ERROR.INVALID
            ? 'no such actor, or the blob does not belong here'
            : status === ERROR.NO_MEMORY
              ? 'out of memory'
              : 'failed';
    return new Error(`${what}: ${why || 'cannot be stored'}`);
  };
  /* (system, actor info) of a session, or null */
  const locate = (sys, sessionID) =>
    scoped(() => {
      const segment = out32();
      const actor = out32();
      const s = session(sessionID);
      if (!s || x.ta_host_locate(sys.host, s, segment, actor) !== OK) return null;
      const system = x.ta_host_system(sys.host, u32(segment));
      const info = zeroed(size('ta_actor_info'));
      setU32(info, size('ta_actor_info'));
      if (!system || x.ta_actor_info_get(system, u32(actor), info) !== OK) return null;
      const A = offsets('ta_actor_info');
      if (u32(info + A.state) === K.ACTOR_FREE) return null;
      return {
        system,
        segment: u32(segment),
        actor: u32(actor),
        state: u32(info + A.state),
        chart: u32(info + A.chart),
        parent: u32(info + A.parent),
        serial: n64(info + A.serial),
        instance: n64(info + A.datamodel_instance),
      };
    });
  const segmentImage = (sys, segment) =>
    scoped(() => {
      const S = offsets('ta_host_segment_info');
      const info = zeroed(size('ta_host_segment_info'));
      setU32(info, size('ta_host_segment_info'));
      return x.ta_host_segment_info_get(sys.host, segment, info) === OK ? { live: !!u32(info + S.live), actors: u32(info + S.actors), current: u32(info + S.current), image: u32(info + S.image) } : null;
    });

  /* the native datamodel's tables as neutral JSON: {"i": [[handle, serial, hash,
   * chart, state], ...], "p": [[handle, refs, value], ...]} */
  const nativeSnapshot = (sys) => {
    const rows = [];
    sys.instances.forEach((instance, i) => {
      if (!instance || !instance.live) return;
      const state = scopeWire(sys, instance.scope);
      if (state === null) throw statusError('snapshot', ERROR.UNSUPPORTED, sys.error);
      rows.push(`[${i + 1},"${instance.serial}","${hashText(instance.chart.hash)}",${instance.chart.chart},${state}]`);
    });
    const payloads = [];
    sys.payloadRefs.forEach((refs, i) => {
      if (!refs) return;
      try {
        payloads.push(`[${i + 1},${refs},${wireValue(sys.payloads[i], 1)}]`);
      } catch {
        throw statusError('snapshot', ERROR.UNSUPPORTED, "a queued event's data is not a neutral value");
      }
    });
    return `{"i":[${rows.join(',')}],"p":[${payloads.join(',')}]}`;
  };
  const chartByHash = (sys, hash, chart) => {
    for (const c of sys.charts.values()) if (c.hash === hash && c.chart === chart) return c;
    return null;
  };
  /* the native datamodel's tables back at their handles */
  const nativeRestore = (sys, json) => {
    const tables = valueOfWire(json, sys.callbacks.ref);
    if (!tables) return ERROR.INVALID;
    for (const [handle, serial, hash, chart, state] of tables.i || []) {
      const c = chartByHash(sys, BigInt(`0x${hash}`), chart);
      if (!c) return ERROR.INVALID;
      const io = {};
      for (const entry of sys.io) io[entry.type] = { location: entry.location };
      const scope = scopeNew(sys, String(serial), undefined, io);
      if (state && state.v) Object.assign(scope, state.v);
      if (state && state.e && typeof state.e === 'object') setEvent(scope, state.e);
      sys.instances[handle - 1] = { live: true, chart: c, serial: Number(serial), scope, input: undefined, stamp: 0, outside: 0, string: 0, nextFree: 0 };
      sys.liveInstances++;
    }
    sys.freeInstance = 0;
    for (let i = sys.instances.length; i-- > 0; )
      if (!sys.instances[i] || !sys.instances[i].live) {
        sys.instances[i] = { live: false, nextFree: sys.freeInstance };
        sys.freeInstance = i + 1;
      }
    for (const [handle, refs, value] of tables.p || []) {
      sys.payloads[handle - 1] = value;
      sys.payloadRefs[handle - 1] = refs;
      sys.livePayloads++;
    }
    sys.freePayload = 0;
    for (let i = sys.payloads.length; i-- > 0; )
      if (!sys.payloadRefs[i]) {
        sys.payloadRefs[i] = 0;
        sys.payloadNext[i] = sys.freePayload;
        sys.freePayload = i + 1;
      }
    return OK;
  };
  /* a snapshot's parts (ta_host_snapshot_unpack): the blob copied into memory, freed by the caller */
  const snapshotIn = (bytes) => {
    const b = asBytes(bytes);
    if (!b) return null;
    const pack = malloc(b.length || 1, 8); /* aligned for the records read in place */
    bytesAt().set(b, pack);
    const C = offsets('ta_host_snapshot_contents');
    const contents = scoped(() => {
      const p = zeroed(size('ta_host_snapshot_contents'));
      setU32(p, size('ta_host_snapshot_contents'));
      if (x.ta_host_snapshot_unpack(pack, b.length, p) !== OK) return null;
      return {
        systemID: view(p + C.system_id),
        ecmascript: view(p + C.datamodel),
        now: n64(p + C.time),
        imageCount: u32(p + C.image_count),
        host: u32(p + C.host),
        hostLength: u32(p + C.host_length),
        state: u32(p + C.state),
        stateLength: u32(p + C.state_length),
      };
    });
    if (!contents) {
      free(pack);
      return null;
    }
    return { pack, length: b.length, ...contents };
  };
  const snapshotImages = (snapshot) =>
    scoped(() => {
      const images = [];
      const length = out32();
      for (let i = 0; i < snapshot.imageCount; i++) {
        const q = x.ta_host_snapshot_image(snapshot.pack, snapshot.length, i, length);
        images.push(q ? new NativeImage(copyOut(q, u32(length))) : null);
      }
      return images;
    });

  function actorsOf(sys) {
    return scoped(() => {
      const out = [];
      const cursor = out64();
      const batch = scratch(8 * 256, 8);
      let n;
      while ((n = x.ta_host_actors(sys.host, cursor, batch, 256)) > 0) for (let i = 0; i < n; i++) out.push(n64(batch + 8 * i));
      return out;
    });
  }

  return {
    /* createSystem(callbacks, config) -> system */
    createSystem(callbacks, config) {
      if (!callbacks || !config) throw new TypeError('createSystem(callbacks, config)');
      const sys = systemNew(callbacks, config);
      const host = scoped(() => {
        const out = out32();
        return x.ta_host_new(sys.config, out) === OK ? u32(out) : 0;
      });
      if (!host) {
        systemDestroy(sys);
        throw new Error('cannot create the system');
      }
      sys.host = host;
      return handleOf(sys);
    },

    /* restoreSystem(callbacks, config, images: [image | null], snapshot) -> system */
    restoreSystem(callbacks, config, images, bytes) {
      const snapshot = snapshotIn(bytes);
      if (!snapshot || !callbacks || !config) throw new TypeError('restoreSystem(callbacks, config, images, snapshot)');
      const list = Array.isArray(images) ? images : [];
      const host = malloc(snapshot.hostLength || 1, 8); /* the snapshot's records are read in place */
      bytesAt().copyWithin(host, snapshot.host, snapshot.host + snapshot.hostLength);
      try {
        /* the images it needs: one per segment, by content hash (0: retired) */
        const hashes = scoped(() => {
          const n = x.ta_host_snapshot_segments(host, snapshot.hostLength, 0, 0);
          const buffer = scratch(8 * (n || 1), 8);
          if (n) x.ta_host_snapshot_segments(host, snapshot.hostLength, buffer, n);
          return Array.from({ length: n }, (_, k) => u64(buffer + 8 * k));
        });
        hashes.forEach((hash, k) => {
          const image = list[k];
          if (hash && (!(image instanceof NativeImage) || imageHash(image.image) !== hash))
            throw new Error(`restore: segment ${k} ran the image ${hashText(hash)}, which is not among the images given`);
        });
        if (!hashes.length) throw new TypeError('restore: not a host snapshot');
        const sys = systemNew(callbacks, config);
        let status;
        let step = "the host's snapshot does not match these images or this build";
        sys.running = 1;
        try {
          status = scoped(() => {
            const array = scratch(4 * (list.length || 1), 4);
            list.forEach((image, k) => {
              setU32(array + 4 * k, image instanceof NativeImage ? image.image : 0);
              if (image instanceof NativeImage) sys.images[k] = image;
            });
            const out = out32();
            const s = x.ta_host_restore(sys.config, array, list.length, host, snapshot.hostLength, out);
            sys.host = s === OK ? u32(out) : 0;
            return s;
          });
          if (status === OK) {
            step = "the datamodel's snapshot does not match";
            if (sys.native) status = nativeRestore(sys, text(snapshot.state, snapshot.stateLength));
            else {
              status = x.ta_datamodel_ecmascript_restore(sys.quickjs, snapshot.state, snapshot.stateLength);
              /* what the binding knows of each session */
              for (const s of status === OK ? actorsOf(sys) : []) {
                const where = locate(sys, s);
                const segment = where && where.instance && segmentImage(sys, where.segment);
                if (segment) sys.sessions.set(where.instance, { serial: where.serial, image: segment.image, chart: where.chart, stamp: 0 });
              }
            }
          }
        } finally {
          sys.running = 0;
        }
        if (status !== OK) {
          systemDestroy(sys);
          throw new Error(`restore: ${step} (${status})`);
        }
        return handleOf(sys);
      } finally {
        free(host);
        free(snapshot.pack);
      }
    },
    /* snapshotContents(blob) -> { systemID, ecmascript, now, images: [image | null] } */
    snapshotContents(bytes) {
      const snapshot = snapshotIn(bytes);
      if (!snapshot) throw new TypeError('not a system snapshot (system.snapshot())');
      try {
        return { systemID: snapshot.systemID, ecmascript: snapshot.ecmascript, now: snapshot.now, images: snapshotImages(snapshot) };
      } finally {
        free(snapshot.pack);
      }
    },

    /* addImage(system, image) -> segment: its charts become the current versions */
    addImage(handle, image) {
      const sys = systemOf(handle);
      imageOf(image);
      const segment = scoped(() => {
        const out = out32();
        /* charts attach: resolve() runs JavaScript */
        return entered(sys, () => x.ta_host_add_image(sys.host, image.image, out)) === OK ? u32(out) : -1;
      });
      if (segment < 0) throw new Error('cannot add the image');
      sys.images[segment] = image;
      return segment;
    },
    /* chart(system, name) -> [segment, chart] of the current version, or null */
    chart(handle, name) {
      const sys = systemOf(handle);
      return scoped(() => {
        const segment = out32();
        const chart = out32();
        return typeof name === 'string' && x.ta_host_chart(sys.host, viewOf(name), segment, chart) === OK ? [u32(segment), u32(chart)] : null;
      });
    },
    /* boot(system, segment): the actors start with the next pump */
    boot(handle, segment) {
      const sys = systemOf(handle);
      x.ta_host_advance_time(sys.host, tickNow(sys));
      if (entered(sys, () => x.ta_host_boot(sys.host, segment >>> 0)) !== OK) throw new Error('cannot boot the image');
    },
    actorByName(handle, name) {
      const sys = systemOf(handle);
      return typeof name === 'string' ? scoped(() => Number(x.ta_host_actor_by_name(sys.host, viewOf(name)))) : 0;
    },
    /* actors(system) -> every session, host-wide */
    actors: (handle) => actorsOf(systemOf(handle)),

    /* spawn(system, segment, chart, name, input, traceparent) -> session: the
     * bootstrap runs with the next pump (the caller knows the session first) */
    spawn(handle, segment, chart, name, inputValue, traceparent) {
      const sys = systemOf(handle);
      const data = payloadOfValue(sys, inputValue);
      return scoped(() => {
        const out = out64();
        x.ta_host_advance_time(sys.host, tickNow(sys));
        const status = x.ta_host_spawn(sys.host, segment >>> 0, chart >>> 0, viewOf(optional(name)), BigInt(data), viewOf(optional(traceparent)), out);
        if (status !== OK) {
          payloadDrop(sys, data);
          throw new Error('cannot spawn the actor');
        }
        return n64(out); /* it starts with the next pump */
      });
    },
    /* send(system, session, name, data, traceparent): one event, then the run loop */
    send(handle, sessionID, name, data, traceparent) {
      const sys = systemOf(handle);
      const payload = payloadOfValue(sys, data);
      scoped(() => {
        const event = eventIn(scratch(size('ta_event_in'), 8), { name: typeof name === 'string' ? name : '', traceparent: optional(traceparent), data: payload });
        if (x.ta_host_send(sys.host, session(sessionID), event) !== OK) payloadDrop(sys, payload);
      });
      return inputDone(sys, undefined);
    },
    /* sendMany(system, sessions, names, datas, traceparents) -> accepted */
    sendMany(handle, sessions, names, datas, traceparents) {
      const sys = systemOf(handle);
      const n = sessions.length;
      const delivered = scoped(() => {
        const ids = scratch(8 * (n || 1), 8);
        const events = scratch(size('ta_event_in') * (n || 1), 8);
        const payloads = [];
        try {
          for (let i = 0; i < n; i++) {
            const payload = payloadOfValue(sys, datas[i]);
            payloads.push(payload);
            setU64(ids + 8 * i, session(sessions[i]));
            eventIn(events + size('ta_event_in') * i, { name: typeof names[i] === 'string' ? names[i] : '', traceparent: optional(traceparents[i]), data: payload });
          }
        } catch (error) {
          for (const p of payloads) payloadDrop(sys, p);
          throw error;
        }
        let accepted = 0;
        let refused = 0;
        while (accepted < n) {
          /* a refused event is skipped (its session is gone) */
          accepted += x.ta_host_send_many(sys.host, ids + 8 * accepted, events + size('ta_event_in') * accepted, n - accepted);
          if (accepted < n) {
            payloadDrop(sys, payloads[accepted]);
            refused++;
            accepted++;
          }
        }
        return n - refused;
      });
      return inputDone(sys, delivered);
    },
    /* post(system, session, { name, data, wire, sendid, origin, origintype, raw, traceparent,
     * flags, token }): an event with every field of ta_event_in */
    post(handle, sessionID, fields) {
      const sys = systemOf(handle);
      const payload = typeof fields.wire === 'string' ? payloadOfWire(sys, fields.wire, false) : payloadOfValue(sys, fields.data);
      scoped(() => {
        const event = eventIn(scratch(size('ta_event_in'), 8), { ...fields, data: payload });
        if (x.ta_host_send(sys.host, session(sessionID), event) !== OK) payloadDrop(sys, payload);
      });
      return inputDone(sys, undefined);
    },
    /* deliver(system, peer, { target, name, data | wire, origin, origintype, sendid, msgid,
     * dedup, hops, traceparent }) -> 'delivered', 'relayed', 'duplicate' or 'refused' */
    deliver(handle, peer, fields) {
      const sys = systemOf(handle);
      const payload = typeof fields.wire === 'string' ? payloadOfWire(sys, fields.wire, false) : payloadOfValue(sys, fields.data);
      const outcome = scoped(() => {
        const V = offsets('ta_envelope');
        const envelope = zeroed(size('ta_envelope'));
        setU32(envelope + V.size, size('ta_envelope'));
        for (const [field, name] of [['target', 'target'], ['name', 'name'], ['origin', 'origin'], ['origintype', 'origintype'], ['sendid', 'sendid'], ['msgid', 'msgid'], ['deduplication_key', 'dedup'], ['traceparent', 'traceparent']])
          if (typeof fields[name] === 'string') putView(envelope + V[field], fields[name]);
        setU32(envelope + V.hops, numberOr0(fields.hops) >>> 0);
        setU64(envelope + V.data, BigInt(payload));
        const out = out32();
        x.ta_host_advance_time(sys.host, tickNow(sys));
        const status = x.ta_host_deliver(sys.host, peer >>> 0, envelope, out);
        if (status !== OK) payloadDrop(sys, payload);
        return status === OK ? u32(out) : -1;
      });
      return inputDone(sys, ['delivered', 'relayed', 'duplicate', 'refused'][outcome] || 'refused');
    },
    /* pump(system, fired) -> run code: run what is due (after a spawn, boot or adopt;
     * when the caller's clock moved); `fired`: callbacks.schedule's timer went off */
    pump(handle, fired) {
      const sys = systemOf(handle);
      if (fired === true) sys.armed = -3; /* the timer is no longer armed */
      return pump(sys);
    },
    /* now(system) -> the system's time (ms) */
    now: (handle) => systemNow(systemOf(handle)),
    later,
    terminate(handle, sessionID) {
      const sys = systemOf(handle);
      return inputDone(sys, x.ta_host_terminate(sys.host, session(sessionID)) === OK);
    },
    /* inspect(system, session) -> { segment, chart, state, parent, states } | null */
    inspect(handle, sessionID) {
      const sys = systemOf(handle);
      const where = locate(sys, sessionID);
      if (!where) return null;
      const states = scoped(() => {
        const buffer = scratch(4 * 256, 4);
        const n = x.ta_actor_config(where.system, where.actor, buffer, 256);
        return Array.from({ length: Math.min(n, 256) }, (_, i) => u32(buffer + 4 * i));
      });
      return { segment: where.segment, chart: where.chart, state: where.state, parent: where.parent, states };
    },
    /* stateValue(system, session) -> its configuration as a state value (ta_host_state_value) */
    stateValue(handle, sessionID) {
      const sys = systemOf(handle);
      const json = scoped(() => {
        const s = session(sessionID);
        let capacity = 512;
        let buffer = scratch(capacity, 1);
        let length = x.ta_host_state_value(sys.host, s, buffer, capacity);
        if (length >= capacity) {
          capacity = length + 1;
          buffer = scratch(capacity, 1);
          length = x.ta_host_state_value(sys.host, s, buffer, capacity);
        }
        return length ? text(buffer, length) : '{}';
      });
      return JSON.parse(json);
    },
    /* context(system, session) -> the session's variables (native: its scope) */
    context(handle, sessionID) {
      const sys = systemOf(handle);
      const where = locate(sys, sessionID);
      if (!where || !where.instance) return undefined;
      if (sys.native) {
        const instance = instanceAt(sys, where.instance);
        return instance ? instance.scope : undefined;
      }
      const segment = segmentImage(sys, where.segment);
      return segment ? quickjsVariables(sys, where.instance, segment.image, where.chart) : undefined;
    },

    /* ---- hibernation, moving actors, migration (src/persist.c) */
    sweep(handle, max) {
      const sys = systemOf(handle);
      max = max >>> 0 || 1 << 30;
      return scoped(() => {
        const capacity = Math.min(max, 65536);
        const buffer = scratch(8 * capacity, 8);
        const n = x.ta_host_sweep(sys.host, max, buffer, capacity);
        return Array.from({ length: n }, (_, i) => n64(buffer + 8 * i));
      });
    },
    /* hibernate(system, session) -> blob; throws (saying why) when it cannot */
    hibernate(handle, sessionID) {
      const sys = systemOf(handle);
      const s = session(sessionID);
      const [status, blob] = writeBlob(sys, (b, c, l) => x.ta_host_hibernate(sys.host, s, b, c, l));
      if (status !== OK) throw statusError('cannot hibernate the actor', status, sys.error);
      const p = parkedAdd(sys, Number(s));
      p.blob = blob;
      p.woken = false;
      return blob;
    },
    /* stored(system, session) -> true when the actor woke meanwhile: delete the store's copy */
    stored(handle, sessionID) {
      const sys = systemOf(handle);
      const key = Number(session(sessionID));
      const p = sys.parked.get(key);
      const stale = !!(p && p.woken);
      if (p && !p.waking) sys.parked.delete(key);
      else if (p) p.blob = null;
      return stale;
    },
    /* hibernated(system, session) -> whether the actor is hibernated (its blob elsewhere) */
    hibernated(handle, sessionID) {
      const where = locate(systemOf(handle), sessionID);
      return !!where && where.state === K.ACTOR_HIBERNATED;
    },
    /* wake(system, session, blob): the actor is back and its work runs */
    wake(handle, sessionID, blob) {
      const sys = systemOf(handle);
      const b = asBytes(blob);
      if (!b) throw new TypeError('wake(system, session, blob: Uint8Array)');
      const key = Number(session(sessionID));
      sys.parked.delete(key);
      const status = wakeFrom(sys, key, b);
      if (status !== OK) throw statusError('cannot wake the actor', status);
      return inputDone(sys, undefined); /* its work runs now */
    },
    /* exportActor(system, session) -> blob: the actor leaves; its delayed sends
     * are due relative to time 0 (ta_blob_rebase), so any clock adopts it */
    exportActor(handle, sessionID) {
      const sys = systemOf(handle);
      const now = tickNow(sys);
      x.ta_host_advance_time(sys.host, now);
      const s = session(sessionID);
      const [status, blob] = writeBlob(sys, (b, c, l) => x.ta_host_export_actor(sys.host, s, b, c, l));
      if (status !== OK) throw statusError('cannot export the actor', status, sys.error);
      return scoped(() => {
        const p = bytesIn(blob);
        const rebased = x.ta_blob_rebase(p, blob.length, now, 0n);
        if (rebased !== OK) throw statusError('cannot export the actor', rebased, sys.error);
        return copyOut(p, blob.length);
      });
    },
    /* adopt(system, blob) -> session: into the newest segment whose image the blob belongs to */
    adopt(handle, blob) {
      const sys = systemOf(handle);
      const b = asBytes(blob);
      if (!b) throw new TypeError('adopt(system, blob: Uint8Array)');
      const now = tickNow(sys);
      let status = OK;
      x.ta_host_advance_time(sys.host, now);
      const adopted = entered(sys, () =>
        scoped(() => {
          const p = bytesIn(b);
          status = x.ta_blob_rebase(p, b.length, 0n, now);
          const out = out64();
          for (let k = x.ta_host_segment_count(sys.host); status === OK && k-- > 0; ) {
            const segment = segmentImage(sys, k);
            if (!segment || !segment.live) continue;
            status = x.ta_host_adopt(sys.host, k, p, b.length, out);
            if (status !== ERROR.INVALID) break;
            status = OK;
          }
          return n64(out);
        }),
      );
      if (status !== OK || !adopted) throw statusError('cannot adopt the actor', status === OK ? ERROR.INVALID : status, '');
      return adopted; /* it goes on with the next pump */
    },
    /* migrate(system, name, session, wake, timeout) -> Promise<{ migrated, failed, pending:
     * [sessions] }>, settled when the job ends (with the next pumps) */
    migrate(handle, name, sessionID, wakeThem, timeout) {
      const sys = systemOf(handle);
      const job = scoped(() => {
        const out = out32();
        const status = x.ta_host_migrate(sys.host, viewOf(optional(name)), session(sessionID), wakeThem === true ? K.HOST_MIGRATE_WAKE : 0, ticks(timeout), out);
        if (status !== OK) throw new Error('cannot start the migration');
        return u32(out);
      });
      return new Promise((resolve) => sys.jobs.set(job, resolve));
    },
    /* uninvoked(system, segment, token): the service ended (done, fail, no invoker) */
    uninvoked(handle, segment, token) {
      const sys = systemOf(handle);
      const inv = invocationAt(sys, segment >>> 0, token >>> 0);
      if (inv) invocationDrop(sys, inv);
    },
    /* invocations(system) -> the invocations kept (to abort when the system stops) */
    invocations: (handle) => systemOf(handle).invocations.map((inv) => inv.object),

    /* snapshot(system) -> blob (ta_host_snapshot_pack; quiescent: no migration) */
    snapshot(handle) {
      const sys = systemOf(handle);
      sys.error = '';
      let datamodel;
      if (sys.native) datamodel = encoder.encode(nativeSnapshot(sys));
      else
        datamodel = scoped(() => {
          const length = out32();
          let status = x.ta_datamodel_ecmascript_snapshot(sys.quickjs, 0, 0, length);
          if (status === OK || status === ERROR.NO_MEMORY) {
            const buffer = scratch(u32(length) + 1, 8);
            status = x.ta_datamodel_ecmascript_snapshot(sys.quickjs, buffer, u32(length) + 1, length);
            if (status === OK) return copyOut(buffer, u32(length));
          }
          const p = x.ta_datamodel_ecmascript_last_error(sys.quickjs);
          let n = 0;
          while (bytesAt()[p + n]) n++;
          throw statusError('snapshot', status, text(p, n));
        });
      return scoped(() => {
        const label = viewOf(sys.native ? 'native' : 'quickjs');
        const state = bytesIn(datamodel);
        const length = out32();
        let status = x.ta_host_snapshot_pack(sys.host, label, state, datamodel.length, 0, 0, length);
        if (status === ERROR.NO_MEMORY) {
          const buffer = scratch(u32(length) || 1, 8);
          status = x.ta_host_snapshot_pack(sys.host, label, state, datamodel.length, buffer, u32(length), length);
          if (status === OK) return copyOut(buffer, u32(length));
        }
        throw statusError('snapshot', status === OK ? ERROR.INVALID : status, sys.error);
      });
    },

    /* stats(system) -> { actors, ..., charts: [...], segments: [...] } */
    stats(handle) {
      const sys = systemOf(handle);
      return scoped(() => {
        const H = offsets('ta_host_stats');
        const host = zeroed(size('ta_host_stats'));
        setU32(host, size('ta_host_stats'));
        x.ta_host_stats_get(sys.host, host);
        const out = {
          actors: u32(host + H.actors),
          hibernated: u32(host + H.hibernated),
          runnable: u32(host + H.runnable),
          jobs: u32(host + H.jobs),
          regionBytes: n64(host + H.region_bytes),
          delivered: n64(host + H.delivered),
          routed: n64(host + H.routed),
          migrated: n64(host + H.migrated),
          retired: n64(host + H.retired),
          sessions: sys.native ? sys.liveInstances : sys.sessions.size,
          payloads: sys.livePayloads,
        };
        if (sys.quickjs) out.heapBytes = Number(x.ta_datamodel_ecmascript_heap_bytes(sys.quickjs));
        const C = offsets('ta_chart_stats');
        const count = x.ta_host_chart_stats(sys.host, 0, 0);
        const charts = zeroed(size('ta_chart_stats') * (count || 1));
        for (let i = 0; i < count; i++) setU32(charts + size('ta_chart_stats') * i, size('ta_chart_stats'));
        const got = x.ta_host_chart_stats(sys.host, charts, count);
        out.charts = Array.from({ length: got }, (_, i) => {
          const c = charts + size('ta_chart_stats') * i;
          return {
            name: view(c + C.name),
            actors: u32(c + C.actors),
            hibernated: u32(c + C.hibernated),
            spawned: n64(c + C.spawned),
            ended: n64(c + C.ended),
            turns: n64(c + C.turns),
            macrosteps: n64(c + C.macrosteps),
            fuel: n64(c + C.fuel),
            errorsExecution: n64(c + C.errors_execution),
            errorsCommunication: n64(c + C.errors_communication),
            errorsPlatform: n64(c + C.errors_platform),
            faults: n64(c + C.faults),
          };
        });
        const S = offsets('ta_system_stats');
        out.segments = [];
        for (let k = 0, n = x.ta_host_segment_count(sys.host); k < n; k++) {
          const segment = segmentImage(sys, k);
          if (!segment || !segment.live) continue;
          const stats = zeroed(size('ta_system_stats'));
          setU32(stats, size('ta_system_stats'));
          x.ta_system_stats_get(x.ta_host_system(sys.host, k), stats);
          out.segments.push({
            segment: k,
            actors: segment.actors,
            current: segment.current,
            actorsCapacity: u32(stats + S.actors_capacity),
            events: u32(stats + S.events),
            eventsCapacity: u32(stats + S.events_capacity),
            timers: u32(stats + S.timers),
            regionBytes: n64(stats + S.region_bytes),
            hibernated: u32(stats + S.hibernated),
          });
        }
        return out;
      });
    },
    destroy(handle) {
      const sys = handle && handle.__system;
      if (!sys) return;
      if (sys.running) {
        /* from an effect handler: the host goes when the pump ends */
        sys.stopping = true;
        return;
      }
      systemDestroy(sys);
    },

    /* ---- links */
    /* linkConnect(system, options) -> link: the next pump asks for its transport (an "open" effect) */
    linkConnect: (handle, options) => linkOpen(handle, options, false),
    /* linkServe(system, options) -> server */
    linkServe: (handle, options) => linkOpen(handle, options, true),
    /* linkAccept(system, server) -> connection: a transport the server took */
    linkAccept(handle, server) {
      const sys = linksCall(handle);
      return scoped(() => {
        const out = out32();
        return x.ta_links_accept(sys.links, server >>> 0, out) === OK ? u32(out) : null;
      });
    },
    /* linkOpened(system, connection) */
    linkOpened(handle, connection) {
      const sys = linksCall(handle);
      x.ta_links_opened(sys.links, connection >>> 0, tickNow(sys));
      return inputDone(sys, undefined);
    },
    /* linkReceived(system, connection, text) -> whether the link is open (its hello came) */
    linkReceived(handle, connection, message) {
      const sys = linksCall(handle);
      const now = tickNow(sys);
      x.ta_host_advance_time(sys.host, now);
      if (typeof message === 'string') scoped(() => x.ta_links_received(sys.links, connection >>> 0, viewOf(message), now));
      const open = scoped(() => {
        const status = zeroed(size('ta_link_status'));
        setU32(status, size('ta_link_status'));
        return x.ta_links_status(sys.links, connection >>> 0, status) === OK && !!u32(status + at('ta_link_status', 'open'));
      });
      return inputDone(sys, open);
    },
    /* linkDropped(system, connection, why) */
    linkDropped(handle, connection, why) {
      const sys = linksCall(handle);
      scoped(() => x.ta_links_dropped(sys.links, connection >>> 0, viewOf(typeof why === 'string' ? why : ''), tickNow(sys)));
      return inputDone(sys, undefined);
    },
    /* linkAdmit(system, connection, verdict): a judged hello (0 refuse, 1 admit, 2 trusted) */
    linkAdmit(handle, connection, verdict) {
      const sys = linksCall(handle);
      x.ta_links_admit(sys.links, connection >>> 0, verdict >>> 0, tickNow(sys));
      return inputDone(sys, undefined);
    },
    /* linkClose(system, handle): a link, a server or a connection */
    linkClose(handle, link) {
      const sys = linksCall(handle);
      x.ta_links_close(sys.links, link >>> 0, tickNow(sys));
      return inputDone(sys, undefined);
    },
    /* linkStatus(system, connection) -> the router's view of its peer with { open,
     * remote, attempts, error }, or null */
    linkStatus(handle, connection) {
      const sys = linksCall(handle);
      const status = scoped(() => {
        const L = offsets('ta_link_status');
        const p = zeroed(size('ta_link_status'));
        setU32(p, size('ta_link_status'));
        if (x.ta_links_status(sys.links, connection >>> 0, p) !== OK) return null;
        return { peer: u32(p + L.peer), open: !!u32(p + L.open), remote: view(p + L.remote), attempts: u32(p + L.attempts), error: viewLength(p + L.error) ? view(p + L.error) : null };
      });
      if (!status) return null;
      const o = peerStatus(sys, status.peer) || {};
      o.open = status.open;
      o.remote = status.remote;
      o.attempts = status.attempts;
      o.error = status.error;
      return o;
    },
    /* linkConnections(system, server) -> its connections that said hello (also while away) */
    linkConnections(handle, server) {
      const sys = linksCall(handle);
      return scoped(() => {
        const n = x.ta_links_connections(sys.links, server >>> 0, 0, 0);
        const buffer = scratch(4 * (n || 1), 4);
        const k = x.ta_links_connections(sys.links, server >>> 0, buffer, n);
        return Array.from({ length: k }, (_, i) => u32(buffer + 4 * i));
      });
    },

    /* ---- tracing */
    /* traceConfigure(system, { enabled, detail, kinds, values, sample, rules, parent, logs,
     * spanMetrics, valueLimit, sink, resource }): what the systems write (ta_host_trace_set)
     * and the encoder that reads it (none when not enabled: then no records are
     * written; traceparents still travel) */
    traceConfigure(handle, o) {
      const sys = systemOf(handle);
      const enabled = o.enabled === true;
      const values = o.values === true;
      const kinds = detailKinds(o.detail, o.kinds, values, true);
      const parts = sampleParts(o.sample);
      let flags = 0;
      if (o.parent !== undefined && o.parent !== 'link' && o.parent !== 'cause') throw new TypeError('parent: "cause" or "link"');
      if (o.parent === 'link') flags |= K.OTLP_LINK_PARENTS;
      if (values) flags |= K.OTLP_VALUES;
      if (o.logs === false) flags |= K.OTLP_NO_LOGS;
      if (o.spanMetrics === true) flags |= K.OTLP_SPAN_METRICS;
      const rules = (Array.isArray(o.rules) ? o.rules : []).map(ruleOf);
      for (const r of rules) if (r.detail !== NIL && r.detail & K.TRACE_VALUES) flags |= K.OTLP_VALUES;
      const status = scoped(() => {
        const P = offsets('ta_host_trace_options');
        const options = zeroed(size('ta_host_trace_options'));
        setU32(options + P.size, size('ta_host_trace_options'));
        setU32(options + P.flags, enabled ? 0 : K.HOST_TRACE_NO_RECORDS);
        setU32(options + P.kinds, kinds);
        setU32(options + P.sample_parts, parts);
        setU32(options + P.sample_whole, SAMPLE_WHOLE);
        setU32(options + P.rules, rulesIn(rules));
        setU32(options + P.rule_count, rules.length);
        return x.ta_host_trace_set(sys.host, options);
      });
      if (status !== OK) throw traceStatusError(status);
      sys.otlpFlags = flags >>> 0;
      sys.traceSink = o.sink === true;
      if (!enabled) {
        if (sys.otlp) free(sys.otlp.memory);
        sys.otlp = null;
        return;
      }
      if (!sys.otlp) {
        sys.otlp = otlpNew({ flags: sys.otlpFlags, value_limit: numberOr0(o.valueLimit) >>> 0 });
        if (!sys.otlp) throw new Error('cannot create the trace encoder');
      }
      encoderFlags(sys);
      if (o.resource !== undefined)
        scoped(() => {
          const [attributes, count] = attributesIn(o.resource);
          x.ta_otlp_set_resource(sys.otlp.otlp, attributes, count);
        });
    },
    /* watch(system, { sessions, names, charts, events, detail, values, perSecond, for: ms })
     * -> id: steps matching any of them are traced in full detail, sampled whatever
     * the ratio, with what they cause, until `for` ms of the system's time passed
     * (15 minutes; Infinity: until unwatch) */
    watch(handle, o) {
      const sys = systemOf(handle);
      const values = o.values === true;
      let kinds = o.detail === undefined ? K.TRACE_FULL : detailKinds(o.detail, undefined, values, false);
      if (values) kinds = (kinds | K.TRACE_VALUES) >>> 0;
      const lists = ['sessions', 'names', 'charts', 'events'].map((name) => (Array.isArray(o[name]) ? o[name] : []));
      const rules = [];
      lists.forEach((list, k) => {
        for (const item of list) {
          const r = { at: K.TRACE_AT_START | K.TRACE_AT_TAKE, from: 0, sample: K.TRACE_SAMPLE, parts: 0, whole: 0, detail: kinds, perSecond: numberOr0(o.perSecond) >>> 0, burst: 0, session: 0n, chart: '', actor: '', event: '' };
          if (k === 0) r.session = decimal(item);
          else r[['', 'actor', 'chart', 'event'][k]] = typeof item === 'string' ? item : '';
          rules.push(r);
        }
      });
      if (!rules.length) throw new TypeError('watchTraces: name actors, sessions, names, charts or events');
      const ms = typeof o.for === 'number' ? o.for : WATCH_MS;
      const until = ms > 0 && ms < 1e15 ? ticks(systemNow(sys) + ms) : NEVER;
      const result = scoped(() => {
        const out = out32();
        const status = x.ta_host_trace_watch(sys.host, rulesIn(rules), rules.length, until, out);
        return status === OK ? { id: u32(out) } : { status };
      });
      if (result.status !== undefined) throw traceStatusError(result.status);
      if (values) sys.watchValues = true;
      encoderFlags(sys);
      return inputDone(sys, result.id); /* its end is a deadline now */
    },
    /* unwatch(system, id): the watch ends now */
    unwatch(handle, id) {
      x.ta_host_trace_unwatch(systemOf(handle).host, id >>> 0);
    },
    /* traceEnd(system) -> { traces, traceLogs }: every open span ends (incomplete), for
     * a system going away */
    traceEnd(handle) {
      const sys = systemOf(handle);
      const out = {};
      if (!sys.otlp) return out;
      traceDrain(sys);
      for (let i = 0; i < 64; i++) {
        x.ta_otlp_end_all(sys.otlp.otlp, traceWall(sys));
        tracePut(sys);
        if (!otlpStats(sys.otlp.otlp).open) break;
      }
      const traces = concat(sys.traces);
      const logs = concat(sys.logs);
      sys.traces = [];
      sys.logs = [];
      exporterAdd(sys, K.OTLP_TRACES, traces);
      exporterAdd(sys, K.OTLP_LOGS, logs);
      if (traces) out.traces = traces;
      if (logs) out.traceLogs = logs;
      return out;
    },

    /* ---- the exporter */
    /* exporter(system, { traces, logs, metrics }): which signals core.mjs posts (none: no exporter) */
    exporter(handle, o) {
      const sys = systemOf(handle);
      let signals = 0;
      SIGNALS.forEach((name, i) => {
        if (o[name] === true) signals |= 1 << i;
      });
      sys.exportSignals = signals;
      if (!signals) {
        if (sys.exporter) x.ta_otlp_batch_free(sys.exporter);
        sys.exporter = 0;
        return;
      }
      if (!sys.exporter)
        sys.exporter = scoped(() => {
          const allocator = zeroed(size('ta_allocator'));
          x.ta_runtime_malloc(allocator);
          const out = out32();
          return x.ta_otlp_batch_new(allocator, out) === OK ? u32(out) : 0;
        });
      if (!sys.exporter) throw new Error('cannot create the exporter');
    },
    /* exportAdd(system, signal, bytes): bytes for the exporter (metrics) */
    exportAdd(handle, signal, bytes) {
      const sys = systemOf(handle);
      const b = asBytes(bytes);
      if (SIGNALS.includes(signal) && b) exporterAdd(sys, SIGNALS.indexOf(signal), b);
      return inputDone(sys, undefined); /* posts due go out */
    },
    /* exportFlush(system): post everything waiting now */
    exportFlush(handle) {
      const sys = systemOf(handle);
      sys.exportFlush = K.OTLP_BATCH_FLUSH;
      return inputDone(sys, undefined);
    },
    /* posted(system, signal, ok) -> the failures so far when one is worth a log line now, else 0 */
    posted(handle, signal, ok) {
      const sys = systemOf(handle);
      const k = SIGNALS.indexOf(signal);
      if (!sys.exporter || k < 0) return 0;
      const now = tickNow(sys);
      x.ta_otlp_batch_done(sys.exporter, k, ok === true ? 1 : 0, now);
      const failures = scoped(() => {
        const out = out64();
        return x.ta_otlp_batch_report(sys.exporter, now, out) ? n64(out) : 0;
      });
      return inputDone(sys, failures); /* what waited goes */
    },
    /* exportStats(system) -> { posted, failed, dropped, waiting, inFlight } (bytes) */
    exportStats: (handle) => exportStats(systemOf(handle)),
    /* exportDrain(system) -> [{ signal, body }]: everything waiting, for a system that goes */
    exportDrain(handle) {
      const sys = systemOf(handle);
      const out = [];
      exporterNext(sys, K.OTLP_BATCH_DRAIN, (post) => out.push(post));
      return out;
    },

    /* ---- metrics */
    /* metricsSnapshot(system, startMs, timeMs) -> { startTime, time, metrics }: the host's
     * counts and the binding's own, as the API's metrics */
    metricsSnapshot(handle, start, time) {
      const sys = systemOf(handle);
      return { startTime: start, time, metrics: metricsValue(metricsRequest(sys, ownMetrics(sys), [], start, time)) };
    },
    /* metricsExport(system, metrics, resource, startMs, timeMs): the host's counts, the
     * binding's own and the application's `metrics` go to the exporter (/v1/metrics) */
    metricsExport(handle, metrics, resource, start, time) {
      const sys = systemOf(handle);
      const bytes = metricsRequest(sys, [...ownMetrics(sys), ...(Array.isArray(metrics) ? metrics : [])], resource, start, time);
      exporterAdd(sys, K.OTLP_METRICS, bytes);
      return inputDone(sys, undefined); /* due posts go out */
    },
  };
}
