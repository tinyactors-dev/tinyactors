/**
 * The web binding's native layer: what core.mjs calls `native`, in
 * JavaScript over the public C API of tinyactors.wasm (the release's
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
    decodeMetrics,
    metricsToOTLP: (metrics, resource, start, time) => metricsRequest(null, metrics, resource, start, time),
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

  /* ---- decoding OTLP (core.mjs shapes the results) */
  const fieldsSize = size('ta_otlp_fields');
  /* a copy of a ta_otlp_fields value in scratch (passed by value) */
  const fieldsCopy = (p) => {
    const q = scratch(fieldsSize, 8);
    bytesAt().copyWithin(q, p, p + fieldsSize);
    return q;
  };
  function decodedValue(v) {
    const kind = u32(v + at('ta_otlp_value', 'kind'));
    switch (kind) {
      case K.OTLP_VALUE_STRING:
        return view(v + at('ta_otlp_value', 'bytes'));
      case K.OTLP_VALUE_BOOL:
        return dv.getBigInt64(v + at('ta_otlp_value', 'integer'), true) !== 0n;
      case K.OTLP_VALUE_INT: {
        fresh();
        const i = dv.getBigInt64(v + at('ta_otlp_value', 'integer'), true);
        return i >= -9007199254740991n && i <= 9007199254740991n ? Number(i) : { int: String(i) };
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
        return { list: decodedAttributes(v + at('ta_otlp_value', 'items')) };
      case K.OTLP_VALUE_BYTES: {
        const b = v + at('ta_otlp_value', 'bytes');
        return { bytes: copyOut(u32(b), u32(b + 4)) };
      }
      default:
        return null;
    }
  }
  function decodedAttributes(fieldsAt) {
    return scoped(() => {
      const fields = fieldsCopy(fieldsAt);
      const list = [];
      const cursor = out32();
      const key = zeroed(8, 4);
      const value = zeroed(size('ta_otlp_value'));
      while (x.ta_otlp_next_attribute(fields, cursor, key, value) === 1) list.push([view(key), decodedValue(value)]);
      return list;
    });
  }
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
        o.kind = u32(span + S.kind);
        o.start = String(u64(span + S.start));
        o.end = String(u64(span + S.end));
        o.status = u32(span + S.status);
        if (viewLength(span + S.status_message)) o.statusMessage = view(span + S.status_message);
        o.attributes = decodedAttributes(span + S.attributes);
        o.events = scoped(() => {
          const events = [];
          const list = fieldsCopy(span + S.events);
          const c = out32();
          const e = zeroed(size('ta_otlp_event'));
          while (x.ta_otlp_next_event(list, c, e) === 1)
            events.push({ name: view(e + at('ta_otlp_event', 'name')), time: String(u64(e + at('ta_otlp_event', 'time'))), attributes: decodedAttributes(e + at('ta_otlp_event', 'attributes')) });
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
        o.resource = decodedAttributes(span + S.resource);
        o.scopeName = view(span + S.scope_name);
        o.scopeVersion = view(span + S.scope_version);
        o.scopeAttributes = decodedAttributes(span + S.scope_attributes);
        o.flags = u32(span + S.flags);
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
  function decodeMetrics(bytes) {
    const b = asBytes(bytes);
    if (!b) throw new TypeError('decodeMetrics(bytes: Uint8Array)');
    return scoped(() => {
      const p = bytesIn(b);
      const cursor = zeroed(size('ta_otlp_cursor'));
      const metric = zeroed(size('ta_otlp_metric_view'));
      const M = offsets('ta_otlp_metric_view');
      const P = offsets('ta_otlp_point_view');
      const metrics = [];
      let got;
      while ((got = x.ta_otlp_next_metric(p, b.length, cursor, metric)) === 1) {
        const kind = u32(metric + M.kind);
        const o = { name: view(metric + M.name), unit: view(metric + M.unit), kind, points: [] };
        const m = mark();
        const list = fieldsCopy(metric + M.points);
        const c = out32();
        const point = zeroed(size('ta_otlp_point_view'));
        while ((got = x.ta_otlp_next_point(list, c, kind, point)) === 1) {
          const q = { attributes: decodedAttributes(point + P.attributes) };
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
              while (x.ta_otlp_next_exemplar(xs, xc, e) === 1)
                exemplars.push({ traceID: traceID(e + E.trace), spanID: spanID(u64(e + E.span)), time: n64(e + E.time) / 1e6, value: f64(e + E.value) });
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
      if (got < 0) throw new Error('decodeMetrics: malformed OTLP protobuf');
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
  /* [[key, value], ...] as ta_otlp_attribute[] in scratch */
  const attributesIn = (pairs) => {
    const list = Array.isArray(pairs) ? pairs : [];
    const p = scratch(size('ta_otlp_attribute') * (list.length || 1), 4);
    list.forEach(([k, v], i) => {
      putView(p + size('ta_otlp_attribute') * i, String(k));
      putView(p + size('ta_otlp_attribute') * i + 8, String(v));
    });
    return [p, list.length];
  };
  const hexPair = (s) => (/^[0-9a-fA-F]{32}$/.test(s) ? [BigInt(`0x${s.slice(0, 16)}`), BigInt(`0x${s.slice(16)}`)] : null);
  const hex64 = (s) => (/^[0-9a-fA-F]{16}$/.test(s) ? BigInt(`0x${s}`) : null);

  /* a metric of core.mjs onto an encoder */
  function metricAdd(otlp, metric, start, time) {
    return scoped(() => {
      const points = Array.isArray(metric.points) ? metric.points : [];
      const PT = offsets('ta_otlp_point');
      const array = zeroed(size('ta_otlp_point') * (points.length || 1));
      points.forEach((point, i) => {
        const p = array + size('ta_otlp_point') * i;
        const [attributes, count] = attributesIn(point.attributes);
        setU32(p + PT.attributes, attributes);
        setU32(p + PT.attribute_count, count);
        if (point.integer) {
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
      setU32(m + M.kind, metric.kind >>> 0);
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
        if (status !== OK) throw new TypeError('metricsToOTLP: a metric is malformed');
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

  return { ...pure, ...systems({ x, K, u8: bytesAt, u32, i32, setU32, u64, n64, setU64, text, copyOut, view, viewLength, malloc, free, allocBytes, scratch, zeroed, mark, release, scoped, utf8, putView, viewOf, bytesIn, out32, out64, asBytes, tableFunctions, NativeImage, imageOf, imageHash, chartSource, sourceName, stringOfImage, traceparentOf, otlpNew, otlpRelocate, otlpStats, attributesIn, metricsRequest, hashText }) };
}

/* ---- systems: a ta_host, its datamodel, its run loop ---------------------------------------- */

function systems(m) {
  const { x, K, u32, i32, setU32, u64, n64, setU64, text, copyOut, view, malloc, free, scratch, zeroed, mark, release, scoped, putView, viewOf, bytesIn, out32, out64, asBytes, tableFunctions, NativeImage, imageOf, imageHash, chartSource, sourceName, stringOfImage, traceparentOf, otlpNew, otlpRelocate, otlpStats, attributesIn, metricsRequest, hashText } = m;
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

  /* ---- system handles */
  const systemOf = (handle, allow = 0) => {
    const sys = handle && handle.__system;
    if (!sys) throw new TypeError('expected a system handle');
    if (!sys.host) throw new Error('the system was stopped');
    if (sys.running && sys.running !== allow) throw new Error('the system is running (calls into it do not nest)');
    return sys;
  };
  const reading = (handle) => systemOf(handle, 2);
  const input = (handle) => systemOf(handle, 3);

  function systemNew(callbacks, options) {
    const id = String(options.systemID || '');
    if (!scoped(() => x.ta_address_system_valid(viewOf(id)))) throw new TypeError('createSystem: systemID is not a valid system id (docs/protocol.md)');
    const key = nextKey++;
    const sys = {
      key,
      host: 0,
      callbacks,
      native: options.ecmascript !== 'quickjs',
      quickjs: 0,
      running: 0,
      keepQuiet: false,
      pump: 0,
      changed: [],
      graves: [],
      failures: [],
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
      error: '',
      otlp: null,
      traces: [],
      logs: [],
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
        setU32(config + at('ta_datamodel_ecmascript_config', 'max_ops'), options.maxOps >>> 0);
        setU32(config + at('ta_datamodel_ecmascript_config', 'memory_limit'), options.memoryLimit >>> 0);
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
    /* I/O processors: descriptors whose strings live as long as the system */
    const io = (options.ioProcessors || []).slice(0, IO_PROCESSORS_MAX);
    const descriptors = malloc(size('ta_io_processor_descriptor') * (io.length || 1), 8);
    sys.blocks.push(descriptors);
    io.forEach((entry, i) => {
      const p = descriptors + size('ta_io_processor_descriptor') * i;
      for (const [field, value] of [['type', entry.type], ['location', entry.location]]) {
        const bytes = encoder.encode(String(value === undefined ? '' : value));
        const q = malloc(bytes.length || 1, 1);
        m.u8().set(bytes, q);
        sys.blocks.push(q);
        setU32(p + at('ta_io_processor_descriptor', field), q);
        setU32(p + at('ta_io_processor_descriptor', field) + 4, bytes.length);
      }
      setU32(p + at('ta_io_processor_descriptor', 'flags'), entry.sync ? K.IO_PROCESSOR_SYNC : 0);
      setU32(p + at('ta_io_processor_descriptor', 'pad'), 0);
      sys.io.push({ sync: !!entry.sync, type: String(entry.type), location: String(entry.location === undefined ? '' : entry.location) });
    });
    const idBytes = encoder.encode(id);
    const idPointer = malloc(idBytes.length || 1, 1);
    m.u8().set(idBytes, idPointer);
    sys.blocks.push(idPointer);
    const config = malloc(size('ta_host_config'), 8);
    sys.blocks.push(config);
    m.u8().fill(0, config, config + size('ta_host_config'));
    const H = offsets('ta_host_config');
    const S = H.system;
    const SC = offsets('ta_system_config');
    setU32(config + H.size, size('ta_host_config'));
    setU32(config + H.system_id, idPointer);
    setU32(config + H.system_id + 4, idBytes.length);
    setU32(config + S + SC.size, size('ta_system_config'));
    setU64(config + S + SC.ticks_per_second, 1000n); /* ms */
    setU32(config + S + SC.trace_rules, 64); /* watches add rules (docs/adr/0007) */
    setU32(config + S + SC.max_actors, options.maxActors > 0 ? options.maxActors : 64);
    setU32(config + S + SC.max_events, options.maxEvents > 0 ? options.maxEvents : 256);
    setU32(config + S + SC.max_timers, options.maxTimers > 0 ? options.maxTimers : 64);
    setU32(config + S + SC.string_bytes, options.stringBytes > 0 ? options.stringBytes : 32768);
    if (Array.isArray(options.seed) && options.seed.length === 2) setU64(config + S + SC.trace_seed, (BigInt(options.seed[0] >>> 0) << 32n) | BigInt(options.seed[1] >>> 0));
    setU32(config + H.datamodels, binding);
    setU32(config + H.datamodel_count, 1);
    setU32(config + H.io_processors, io.length ? descriptors : 0);
    setU32(config + H.io_processor_count, io.length);
    x.ta_runtime_malloc(config + H.allocator);
    sys.config = config;
    return sys;
  }

  function systemDestroy(sys) {
    if (!sys.host && !sys.binding) return;
    sys.keepQuiet = true;
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
  }
  const handleOf = (sys) => ({ __system: sys });

  /* ---- tracing */
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
            m.u8().copyWithin(input_ + I.view, view_, view_ + size('ta_trace_view'));
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

  /* ---- the run loop */
  const effectBase = (kind, e) => ({ kind, session: n64(e + EFFECT.session) });
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
  const traceparentAt = (e) => traceparentOf(e + EFFECT.trace);
  function effectOut(sys, e, list, syncList) {
    const kind = u32(e + EFFECT.kind);
    switch (kind) {
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
        /* a synchronous processor's send is answered before the next run */
        (io < sys.io.length && sys.io[io].sync ? syncList : list).push({
          kind: 'send',
          session: n64(e + EFFECT.session),
          root: n64(e + EFFECT.root),
          segment: u32(e + EFFECT.segment),
          token: u32(e + EFFECT.token),
          ioProcessor: io === NIL ? -1 : io,
          target: view(e + EFFECT.target),
          event: view(e + EFFECT.event),
          sendid: view(e + EFFECT.sendid),
          data: valueOfPayload(sys, n64(e + EFFECT.payload), u32(e + EFFECT.datamodel)),
          traceparent: traceparentAt(e),
        });
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
        });
        break;
      case K.EFFECT_UNINVOKE:
        list.push({ kind: 'uninvoke', session: n64(e + EFFECT.session), segment: u32(e + EFFECT.segment), token: u32(e + EFFECT.token) });
        break;
      case K.EFFECT_WAKE:
        list.push(effectBase('wake', e));
        break;
      case K.EFFECT_REQUEST:
        answerRequest(sys, e);
        break;
      case K.HOST_EFFECT_MIGRATED:
        list.push({ kind: 'migrated', session: n64(e + EFFECT.session), segment: u32(e + EFFECT.segment), chart: u32(e + EFFECT.chart), job: u32(e + EFFECT.job) });
        break;
      case K.HOST_EFFECT_JOB_DONE: {
        const job = u32(e + EFFECT.job);
        const pending = scoped(() => {
          const n = x.ta_host_job_pending(sys.host, job, 0, 0);
          const buffer = scratch(8 * (n || 1), 8);
          const k = x.ta_host_job_pending(sys.host, job, buffer, n);
          return Array.from({ length: k }, (_, i) => n64(buffer + 8 * i));
        });
        list.push({ kind: 'job', job, migrated: u32(e + EFFECT.migrated), failed: u32(e + EFFECT.failed), pending });
        break;
      }
      case K.HOST_EFFECT_SEGMENT_FREED: {
        const segment = u32(e + EFFECT.segment);
        sys.images[segment] = undefined;
        list.push({ kind: 'freed', segment });
        break;
      }
      case K.HOST_EFFECT_PEER: {
        /* an envelope for a transport, as neutral JSON */
        const data = wireOfPayload(sys, n64(e + EFFECT.payload), u32(e + EFFECT.datamodel));
        const out = { kind: 'peer', peer: u32(e + EFFECT.peer), origin: view(e + EFFECT.auxiliary), msgid: view(e + EFFECT.msgid), sendid: view(e + EFFECT.sendid), target: view(e + EFFECT.target) };
        scoped(() => {
          const V = offsets('ta_envelope');
          const envelope = zeroed(size('ta_envelope'));
          setU32(envelope + V.size, size('ta_envelope'));
          setU32(envelope + V.hops, u32(e + EFFECT.flags));
          for (const [to, from] of [['target', 'target'], ['name', 'event'], ['sendid', 'sendid'], ['origin', 'auxiliary'], ['msgid', 'msgid']]) {
            setU32(envelope + V[to], u32(e + EFFECT[from]));
            setU32(envelope + V[to] + 4, u32(e + EFFECT[from] + 4));
          }
          putView(envelope + V.traceparent, traceparentAt(e) || '');
          const encoded = viewOf(data || '');
          const length = x.ta_host_envelope_json(envelope, encoded, 0, 0);
          const buffer = scratch(length + 1, 1);
          x.ta_host_envelope_json(envelope, encoded, buffer, length + 1);
          out.envelope = text(buffer, length);
        });
        if (n64(e + EFFECT.payload) && !data) out.malformed = true;
        list.push(out);
        break;
      }
      case K.HOST_EFFECT_PEER_ERROR:
        list.push({ kind: 'peerError', peer: u32(e + EFFECT.peer), origin: view(e + EFFECT.auxiliary), msgid: view(e + EFFECT.msgid), sendid: view(e + EFFECT.sendid), reason: u32(e + EFFECT.token) });
        break;
      default:
        break;
    }
  }

  /* Run until idle (or RUN_ROUNDS runs); every effect, the changed sessions and
   * the traces go to callbacks.effects at once. Returns the run code. */
  function pump(sys, now) {
    if (sys.running === 3) return -2; /* inside a synchronous send: the pump goes on */
    x.ta_host_advance_time(sys.host, BigInt(Math.max(0, Math.trunc(now || 0))));
    sys.running = 1;
    sys.pump++;
    sys.changed = [];
    const list = [];
    let thrown;
    let status = K.RUN_IDLE;
    let deadline = 0n;
    if (sys.quickjs) x.ta_datamodel_ecmascript_wasm_set_time(Number(traceWall(sys)) / 1e6); /* the module has no clock of its own */
    const m0 = mark();
    const run = scratch(size('ta_run'), 8);
    const effects = out32();
    try {
      for (let round = 0; round < RUN_ROUNDS; round++) {
        if (sys.otlp) x.ta_otlp_mark(sys.otlp.otlp, traceFuel(sys), traceWall(sys));
        x.ta_host_run(run, sys.host, RUN_BUDGET);
        status = u32(run + at('ta_run', 'status'));
        deadline = u64(run + at('ta_run', 'next_deadline'));
        if (sys.otlp) x.ta_otlp_mark(sys.otlp.otlp, traceFuel(sys), traceWall(sys));
        traceDrain(sys); /* before the effects: what they cause comes after */
        const syncList = [];
        const n = x.ta_host_effects(sys.host, effects);
        const base = u32(effects);
        for (let i = 0; i < n; i++) effectOut(sys, base + EFFECT_SIZE * i, list, syncList);
        x.ta_host_effects_ack(sys.host);
        /* TA_RUN_EFFECTS: the senders wait; callbacks.sync answers each send now,
         * feeding the host (send, post, deliver) before the next run */
        sys.running = 3;
        for (const e of syncList) {
          try {
            sys.callbacks.sync(e);
          } catch (error) {
            thrown = error;
            break;
          }
        }
        sys.running = 1;
        if (thrown !== undefined || status === K.RUN_IDLE || status === K.RUN_FAULT) break;
      }
    } finally {
      release(m0);
      sys.running = 0;
    }
    for (const [serial, error] of sys.failures) list.push({ kind: 'exception', session: serial, error });
    sys.failures = [];
    sys.graves = [];
    if (sys.otlp) tracePut(sys); /* the spans that ended in this call */
    const traces = concat(sys.traces);
    const logs = concat(sys.logs);
    sys.traces = [];
    sys.logs = [];
    const changed = sys.changed.length ? sys.changed : undefined;
    if (list.length || changed || traces || logs) {
      sys.running = 2; /* the sink may not call in: core.mjs queues */
      try {
        sys.callbacks.effects(list.length ? list : undefined, changed, traces, logs);
      } finally {
        sys.running = 0;
      }
    }
    if (thrown !== undefined) throw thrown;
    if (status !== K.RUN_IDLE) return -2;
    return deadline === 0xffffffffffffffffn ? -1 : Number(deadline);
  }

  /* an event's fields at p (a ta_event_in) */
  const eventIn = (p, fields) => {
    m.u8().fill(0, p, p + size('ta_event_in'));
    setU32(p + EVENT_IN.size, size('ta_event_in'));
    for (const field of ['name', 'sendid', 'origin', 'origintype', 'raw', 'traceparent']) if (typeof fields[field] === 'string') putView(p + EVENT_IN[field], fields[field]);
    setU32(p + EVENT_IN.flags, fields.flags >>> 0);
    setU32(p + EVENT_IN.token, fields.token >>> 0);
    setU64(p + EVENT_IN.data, fields.data || 0);
    return p;
  };
  const session = (v) => BigInt(Math.trunc(Number(v) || 0));
  const time = (now) => BigInt(Math.max(0, Math.trunc(Number(now) || 0)));

  /* a blob through a call that sizes it first (length = needed on TA_ERROR_NO_MEMORY) */
  const writeBlob = (sys, writer) => {
    sys.error = '';
    sys.keepQuiet = true;
    sys.running = 1;
    try {
      return scoped(() => {
        let capacity = 4096;
        const length = out32();
        for (let attempt = 0; attempt < 8; attempt++) {
          const buffer = scratch(capacity, 8);
          const status = writer(buffer, capacity, length);
          if (status === OK) return [OK, buffer, u32(length)];
          if (status !== ERROR.NO_MEMORY) return [status];
          capacity = Math.max(u32(length), capacity * 2);
        }
        return [ERROR.NO_MEMORY];
      });
    } finally {
      sys.running = 0;
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
      if (!sessionID || x.ta_host_locate(sys.host, session(sessionID), segment, actor) !== OK) return null;
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
      const info = zeroed(size('ta_host_segment_info'));
      setU32(info, size('ta_host_segment_info'));
      return x.ta_host_segment_info_get(sys.host, segment, info) === OK ? { live: !!u32(info + 4), actors: u32(info + 8), current: u32(info + 12), image: u32(info + 16) } : null;
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

  return {
    /* createSystem(callbacks, config) -> system */
    createSystem(callbacks, config) {
      const sys = systemNew(callbacks, config);
      const host = out32();
      if (x.ta_host_new(sys.config, host) !== OK) {
        systemDestroy(sys);
        throw new Error('cannot create the system');
      }
      sys.host = u32(host);
      return handleOf(sys);
    },

    /* restoreSystem(callbacks, config, images: [image | null], host, datamodel) -> system */
    restoreSystem(callbacks, config, images, hostBytes, datamodelBytes) {
      const hb = asBytes(hostBytes);
      const db = asBytes(datamodelBytes);
      if (!hb || !db) throw new TypeError('restoreSystem(callbacks, config, images, host, datamodel)');
      const snapshot = malloc(hb.length, 8); /* the snapshot's records are read in place */
      m.u8().set(hb, snapshot);
      try {
        const hashes = scoped(() => {
          const n = x.ta_host_snapshot_segments(snapshot, hb.length, 0, 0);
          const buffer = scratch(8 * (n || 1), 8);
          if (n) x.ta_host_snapshot_segments(snapshot, hb.length, buffer, n);
          return Array.from({ length: n }, (_, k) => u64(buffer + 8 * k));
        });
        if (!hashes.length) throw new TypeError('restore: not a host snapshot');
        hashes.forEach((hash, k) => {
          const image = images[k];
          if (hash && (!(image instanceof NativeImage) || imageHash(image.image) !== hash))
            throw new Error(`restore: segment ${k} ran the image ${hashText(hash)}, which is not among the images given`);
        });
        const sys = systemNew(callbacks, config);
        let status;
        let step = "the host's snapshot does not match these images or this build";
        sys.running = 1;
        try {
          status = scoped(() => {
            const list = scratch(4 * (images.length || 1), 4);
            images.forEach((image, k) => {
              setU32(list + 4 * k, image instanceof NativeImage ? image.image : 0);
              if (image instanceof NativeImage) sys.images[k] = image;
            });
            const host = out32();
            const s = x.ta_host_restore(sys.config, list, images.length, snapshot, hb.length, host);
            sys.host = u32(host);
            return s;
          });
          if (status === OK) {
            step = "the datamodel's snapshot does not match";
            if (sys.native) status = nativeRestore(sys, decoder.decode(db));
            else {
              status = scoped(() => {
                const p = bytesIn(db);
                return x.ta_datamodel_ecmascript_restore(sys.quickjs, p, db.length);
              });
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
        free(snapshot);
      }
    },

    /* addImage(system, image) -> segment: its charts become the current versions */
    addImage(handle, image) {
      const sys = systemOf(handle);
      imageOf(image);
      sys.running = 1; /* charts attach: resolve() runs JavaScript */
      let status;
      const segment = out32();
      try {
        status = x.ta_host_add_image(sys.host, image.image, segment);
      } finally {
        sys.running = 0;
      }
      if (status !== OK) throw new Error('cannot add the image');
      sys.images[u32(segment)] = image;
      return u32(segment);
    },
    /* chart(system, name) -> [segment, chart] of the current version, or null */
    chart(handle, name) {
      const sys = systemOf(handle);
      return scoped(() => {
        const segment = out32();
        const chart = out32();
        return x.ta_host_chart(sys.host, viewOf(String(name)), segment, chart) === OK ? [u32(segment), u32(chart)] : null;
      });
    },
    boot(handle, segment, now) {
      const sys = systemOf(handle);
      sys.running = 1;
      let status;
      try {
        x.ta_host_advance_time(sys.host, time(now));
        status = x.ta_host_boot(sys.host, segment >>> 0);
      } finally {
        sys.running = 0;
      }
      if (status !== OK) throw new Error('cannot boot the image');
    },
    actorByName: (handle, name) => {
      const sys = input(handle);
      return scoped(() => Number(x.ta_host_actor_by_name(sys.host, viewOf(String(name)))));
    },
    actors: (handle) => actorsOf(systemOf(handle)),

    /* spawn(system, segment, chart, name, input, traceparent, now) -> session */
    spawn(handle, segment, chart, name, inputValue, traceparent, now) {
      const sys = systemOf(handle);
      const data = payloadOfValue(sys, inputValue);
      return scoped(() => {
        const out = out64();
        x.ta_host_advance_time(sys.host, time(now));
        const status = x.ta_host_spawn(sys.host, segment >>> 0, chart >>> 0, viewOf(typeof name === 'string' ? name : undefined), BigInt(data), viewOf(typeof traceparent === 'string' ? traceparent : undefined), out);
        if (status !== OK) {
          payloadDrop(sys, data);
          throw new Error('cannot spawn the actor');
        }
        return n64(out);
      });
    },
    /* send(system, session, name, data, traceparent, now) -> run code */
    send(handle, sessionID, name, data, traceparent, now) {
      const sys = input(handle);
      const payload = payloadOfValue(sys, data);
      scoped(() => {
        const event = eventIn(scratch(size('ta_event_in'), 8), { name: String(name), traceparent: typeof traceparent === 'string' ? traceparent : undefined, data: payload });
        if (x.ta_host_send(sys.host, session(sessionID), event) !== OK) payloadDrop(sys, payload);
      });
      return pump(sys, now);
    },
    /* sendMany(system, sessions, names, datas, traceparents, now) -> [accepted, run code] */
    sendMany(handle, sessions, names, datas, traceparents, now) {
      const sys = input(handle);
      const n = sessions.length;
      let delivered = 0;
      scoped(() => {
        const ids = scratch(8 * (n || 1), 8);
        const events = scratch(size('ta_event_in') * (n || 1), 8);
        const payloads = [];
        try {
          for (let i = 0; i < n; i++) {
            const payload = payloadOfValue(sys, datas[i]);
            payloads.push(payload);
            setU64(ids + 8 * i, session(sessions[i]));
            eventIn(events + size('ta_event_in') * i, { name: String(names[i]), traceparent: typeof traceparents[i] === 'string' ? traceparents[i] : undefined, data: payload });
          }
        } catch (error) {
          for (const p of payloads) payloadDrop(sys, p);
          throw error;
        }
        let accepted = 0;
        const refused = new Set();
        while (accepted < n) {
          /* a refused event is skipped (its session is gone) */
          accepted += x.ta_host_send_many(sys.host, ids + 8 * accepted, events + size('ta_event_in') * accepted, n - accepted);
          if (accepted < n) {
            payloadDrop(sys, payloads[accepted]);
            refused.add(accepted);
            accepted++;
          }
        }
        delivered = n - refused.size;
      });
      return [delivered, pump(sys, now)];
    },
    /* post(system, session, { name, data | wire, sendid, origin, origintype, raw, traceparent, flags, token }, now) -> run code */
    post(handle, sessionID, fields, now) {
      const sys = input(handle);
      const payload = typeof fields.wire === 'string' ? payloadOfWire(sys, fields.wire, false) : payloadOfValue(sys, fields.data);
      scoped(() => {
        const event = eventIn(scratch(size('ta_event_in'), 8), { ...fields, data: payload });
        if (x.ta_host_send(sys.host, session(sessionID), event) !== OK) payloadDrop(sys, payload);
      });
      return pump(sys, now);
    },
    /* deliver(system, peer, { target, name, data | wire, origin, origintype, sendid, msgid, dedup, hops, traceparent }, now) -> [outcome, run code] */
    deliver(handle, peer, fields, now) {
      const sys = input(handle);
      const payload = typeof fields.wire === 'string' ? payloadOfWire(sys, fields.wire, false) : payloadOfValue(sys, fields.data);
      const outcome = scoped(() => {
        const V = offsets('ta_envelope');
        const envelope = zeroed(size('ta_envelope'));
        setU32(envelope + V.size, size('ta_envelope'));
        for (const [field, name] of [['target', 'target'], ['name', 'name'], ['origin', 'origin'], ['origintype', 'origintype'], ['sendid', 'sendid'], ['msgid', 'msgid'], ['deduplication_key', 'dedup'], ['traceparent', 'traceparent']])
          if (typeof fields[name] === 'string') putView(envelope + V[field], fields[name]);
        setU32(envelope + V.hops, fields.hops >>> 0);
        setU64(envelope + V.data, payload);
        const out = out32();
        x.ta_host_advance_time(sys.host, time(now));
        const status = x.ta_host_deliver(sys.host, peer >>> 0, envelope, out);
        if (status !== OK) payloadDrop(sys, payload);
        return status === OK ? u32(out) : -1;
      });
      return [outcome, pump(sys, now)];
    },
    pump: (handle, now) => pump(systemOf(handle), now),
    terminate: (handle, sessionID) => {
      const sys = systemOf(handle);
      return x.ta_host_terminate(sys.host, session(sessionID)) === OK;
    },
    /* inspect(system, session) -> { segment, chart, state, parent, states } | null */
    inspect(handle, sessionID) {
      const sys = reading(handle);
      const where = locate(sys, sessionID);
      if (!where) return null;
      const states = scoped(() => {
        const buffer = scratch(4 * 256, 4);
        const n = x.ta_actor_config(where.system, where.actor, buffer, 256);
        return Array.from({ length: Math.min(n, 256) }, (_, i) => u32(buffer + 4 * i));
      });
      return { segment: where.segment, chart: where.chart, state: where.state, parent: where.parent, states };
    },
    /* context(system, session) -> the session's variables (native: its scope) */
    context(handle, sessionID) {
      const sys = reading(handle);
      const where = locate(sys, sessionID);
      if (!where || !where.instance) return undefined;
      if (sys.native) {
        const instance = instanceAt(sys, where.instance);
        return instance ? instance.scope : undefined;
      }
      const segment = segmentImage(sys, where.segment);
      return segment ? quickjsVariables(sys, where.instance, segment.image, where.chart) : undefined;
    },

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
      const [status, buffer, length] = writeBlob(sys, (b, c, l) => x.ta_host_hibernate(sys.host, session(sessionID), b, c, l));
      if (status !== OK) throw statusError('cannot hibernate the actor', status, sys.error);
      return copyOut(buffer, length);
    },
    /* wake(system, session, blob): the actor is back; its work runs with the next pump */
    wake(handle, sessionID, blob) {
      const sys = systemOf(handle);
      const b = asBytes(blob);
      if (!b) throw new TypeError('wake(system, session, blob: Uint8Array)');
      sys.running = 1;
      let status;
      try {
        status = scoped(() => x.ta_host_wake(sys.host, session(sessionID), bytesIn(b), b.length));
      } finally {
        sys.running = 0;
      }
      if (status !== OK) throw statusError('cannot wake the actor', status);
    },
    /* exportActor(system, session, now) -> blob: its delayed sends are due relative to time 0 */
    exportActor(handle, sessionID, now) {
      const sys = systemOf(handle);
      x.ta_host_advance_time(sys.host, time(now));
      const [status, buffer, length] = writeBlob(sys, (b, c, l) => x.ta_host_export_actor(sys.host, session(sessionID), b, c, l));
      if (status !== OK) throw statusError('cannot export the actor', status, sys.error);
      return scoped(() => {
        const p = bytesIn(copyOut(buffer, length));
        const rebased = x.ta_blob_rebase(p, length, time(now), 0n);
        if (rebased !== OK) throw statusError('cannot export the actor', rebased, sys.error);
        return copyOut(p, length);
      });
    },
    /* adopt(system, blob, now) -> [session, run code]: into the newest segment whose image the blob belongs to */
    adopt(handle, blob, now) {
      const sys = systemOf(handle);
      const b = asBytes(blob);
      if (!b) throw new TypeError('adopt(system, blob: Uint8Array, now)');
      let adopted = 0;
      let status = OK;
      x.ta_host_advance_time(sys.host, time(now));
      sys.running = 1;
      try {
        scoped(() => {
          const p = bytesIn(b);
          status = x.ta_blob_rebase(p, b.length, 0n, time(now));
          const out = out64();
          for (let k = x.ta_host_segment_count(sys.host); status === OK && k-- > 0; ) {
            const segment = segmentImage(sys, k);
            if (!segment || !segment.live) continue;
            status = x.ta_host_adopt(sys.host, k, p, b.length, out);
            if (status !== ERROR.INVALID) break;
            status = OK;
          }
          adopted = n64(out);
        });
      } finally {
        sys.running = 0;
      }
      if (status !== OK || !adopted) throw statusError('cannot adopt the actor', status === OK ? ERROR.INVALID : status, '');
      return [adopted, pump(sys, now)];
    },
    /* migrate(system, name, session, wake, timeout) -> job */
    migrate(handle, name, sessionID, wakeThem, timeout) {
      const sys = systemOf(handle);
      return scoped(() => {
        const job = out32();
        const status = x.ta_host_migrate(sys.host, viewOf(typeof name === 'string' ? name : undefined), session(sessionID), wakeThem ? K.HOST_MIGRATE_WAKE : 0, time(timeout), job);
        if (status !== OK) throw new Error('cannot start the migration');
        return u32(job);
      });
    },

    /* snapshot(system) -> { host, datamodel, segments: [image | null] } (quiescent: no migration) */
    snapshot(handle) {
      const sys = systemOf(handle);
      const host = scoped(() => {
        const length = out32();
        let status = x.ta_host_snapshot(sys.host, 0, 0, length);
        if (status !== OK && status !== ERROR.NO_MEMORY) throw statusError('snapshot', status, '');
        const buffer = scratch(u32(length), 8);
        status = x.ta_host_snapshot(sys.host, buffer, u32(length), length);
        if (status !== OK) throw statusError('snapshot', status, '');
        return copyOut(buffer, u32(length));
      });
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
          throw statusError('snapshot', status, scoped(() => {
            const p = x.ta_datamodel_ecmascript_last_error(sys.quickjs);
            let n = 0;
            while (m.u8()[p + n]) n++;
            return text(p, n);
          }));
        });
      const segments = [];
      for (let k = 0, n = x.ta_host_segment_count(sys.host); k < n; k++) {
        const segment = segmentImage(sys, k);
        segments.push(segment && segment.live && sys.images[k] ? sys.images[k] : null);
      }
      return { host, datamodel, segments };
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
      if (sys.running) throw new Error('the system is running');
      systemDestroy(sys);
    },

    /* ---- the router: peers, routes, policy */
    /* peerAdd(host, id, reach[], flags, maxQueue, queueTTLMs) -> peer handle (0: refused) */
    peerAdd(handle, id, reach, flags, maxQueue, queueTTL) {
      const sys = systemOf(handle);
      return scoped(() => {
        const P = offsets('ta_peer_config');
        const c = zeroed(size('ta_peer_config'));
        const list = Array.isArray(reach) ? reach : [];
        const views = scratch(8 * (list.length || 1), 4);
        list.forEach((r, i) => putView(views + 8 * i, String(r)));
        setU32(c + P.size, size('ta_peer_config'));
        setU32(c + P.flags, flags >>> 0);
        putView(c + P.id, String(id));
        setU32(c + P.reach, views);
        setU32(c + P.reach_count, list.length);
        setU32(c + P.max_queue, maxQueue >>> 0);
        setU64(c + P.queue_ttl, time(queueTTL));
        const peer = out32();
        return x.ta_host_peer_add(sys.host, c, peer) === OK ? u32(peer) : 0;
      });
    },
    peerUp: (handle, peer, up) => x.ta_host_peer_up(systemOf(handle).host, peer >>> 0, up ? 1 : 0) === OK,
    peerRemove: (handle, peer) => x.ta_host_peer_remove(systemOf(handle).host, peer >>> 0) === OK,
    peerReach(handle, peer, reach) {
      const sys = systemOf(handle);
      return scoped(() => {
        const list = Array.isArray(reach) ? reach : [];
        const views = scratch(8 * (list.length || 1), 4);
        list.forEach((r, i) => putView(views + 8 * i, String(r)));
        return x.ta_host_peer_reach(sys.host, peer >>> 0, views, list.length) === OK;
      });
    },
    route: (handle, system, peer) => scoped(() => x.ta_host_route(systemOf(handle).host, viewOf(String(system)), peer >>> 0) === OK),
    exportName: (handle, peer, name, events) => scoped(() => x.ta_host_export(systemOf(handle).host, peer >>> 0, viewOf(String(name)), viewOf(String(events))) === OK),
    peerError: (handle, peer, origin, msgid, sendid, reason) =>
      scoped(() => x.ta_host_peer_error(systemOf(handle).host, peer >>> 0, viewOf(origin == null ? '' : String(origin)), viewOf(msgid == null ? '' : String(msgid)), viewOf(sendid == null ? '' : String(sendid)), reason >>> 0) === OK),
    peerRoundTripTime: (handle, peer, ms) => {
      x.ta_host_peer_round_trip_time(systemOf(handle).host, peer >>> 0, ms >>> 0);
    },
    /* peerStatus(host, peer) -> { id, state, queued, rtt, lag, sent, received, errors, duplicates } | null */
    peerStatus(handle, peer) {
      const sys = systemOf(handle);
      return scoped(() => {
        const P = offsets('ta_peer_status');
        const s = zeroed(size('ta_peer_status'));
        setU32(s, size('ta_peer_status'));
        if (x.ta_host_peer_status(sys.host, peer >>> 0, s) !== OK) return null;
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
    },

    /* ---- tracing and metrics */
    /* traceConfigure(host, { enabled, classes, sampleParts, sampleWhole, rules, flags, valueLimit, resource }) */
    traceConfigure(handle, options) {
      const sys = systemOf(handle);
      const enabled = !!options.enabled;
      let parts = options.sampleParts === undefined ? 1 : options.sampleParts >>> 0;
      let whole = options.sampleWhole === undefined ? 1 : options.sampleWhole >>> 0;
      if (!whole) [parts, whole] = [0, 1]; /* nothing sampled */
      const status = scoped(() => {
        const P = offsets('ta_trace_policy');
        const policy = zeroed(size('ta_trace_policy'));
        x.ta_trace_policy_default(policy);
        setU32(policy + P.sample_parts, parts);
        setU32(policy + P.sample_whole, whole);
        const classes = Array.isArray(options.classes) ? options.classes : [];
        for (let i = 0; i < K.TRACE_DETAILS; i++) {
          const kinds = enabled ? classes[i] >>> 0 : 0;
          setU32(policy + P.kinds + 4 * i, kinds || 1 << K.TRACE_DROPPED);
        }
        const rules = Array.isArray(options.rules) ? options.rules : [];
        const R = offsets('ta_trace_rule');
        const array = zeroed(size('ta_trace_rule') * (rules.length || 1));
        rules.forEach((rule, i) => {
          const r = array + size('ta_trace_rule') * i;
          setU32(r + R.size, size('ta_trace_rule'));
          setU32(r + R.at, rule.at >>> 0);
          for (const field of ['chart', 'actor', 'event']) putView(r + R[field], typeof rule[field] === 'string' ? rule[field] : '');
          if (rule.session !== undefined) setU64(r + R.session, BigInt(String(rule.session).replace(/[^0-9].*$/, '') || 0));
          setU32(r + R.from, rule.from >>> 0);
          setU32(r + R.sample, rule.sample >>> 0);
          setU32(r + R.sample_parts, rule.parts >>> 0);
          setU32(r + R.sample_whole, rule.whole >>> 0);
          setU32(r + R.detail, rule.detail < 0 || rule.detail === undefined ? NIL : rule.detail >>> 0);
          setU32(r + R.per_second, rule.perSecond >>> 0);
          setU32(r + R.burst, rule.burst >>> 0);
        });
        setU32(policy + P.rules, array);
        setU32(policy + P.rule_count, rules.length);
        return x.ta_host_trace_set(sys.host, policy);
      });
      if (status === ERROR.NO_MEMORY) throw new Error('traces: more rules than a system holds (64), or no memory');
      if (status === ERROR.INVALID) throw new Error('traces: a rule is malformed, or names more than 16 actors');
      if (!enabled) {
        if (sys.otlp) free(sys.otlp.memory);
        sys.otlp = null;
        return;
      }
      if (!sys.otlp) {
        sys.otlp = otlpNew({ flags: options.flags >>> 0, value_limit: options.valueLimit >>> 0 });
        if (!sys.otlp) throw new Error('cannot create the trace encoder');
      }
      x.ta_otlp_set_flags(sys.otlp.otlp, options.flags >>> 0);
      if (options.resource)
        scoped(() => {
          const [attributes, count] = attributesIn(options.resource);
          x.ta_otlp_set_resource(sys.otlp.otlp, attributes, count);
        });
    },
    /* traceEnd(host) -> { traces, traceLogs }: every open span ends (incomplete) */
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
      if (traces) out.traces = traces;
      if (logs) out.traceLogs = logs;
      sys.traces = [];
      sys.logs = [];
      return out;
    },
    /* metrics(host, metrics, resource, startMs, timeMs) -> Uint8Array */
    metrics: (handle, metrics, resource, start, timeMs) => metricsRequest(systemOf(handle), metrics, resource, start, timeMs),
  };

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
}
