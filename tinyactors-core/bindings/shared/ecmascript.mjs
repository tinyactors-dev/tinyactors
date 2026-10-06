/**
 * The SCXML ECMAScript datamodel (SCXML Appendix B.2) for the JS bindings,
 * value-level: the backends (bindings/node/src/addon.c, bindings/web) forward
 * the core's datamodel calls here with plain JS values; payload handles stay
 * theirs (one table shared with the xstate datamodel, so machines and SCXML
 * documents exchange events directly).
 *
 * The same semantics as the C datamodel (src/datamodel_ecmascript, quickjs-ng) in its
 * shared realm: each session's variables live in a scope object (null
 * prototype: only its own names shadow the globals), expressions are compiled
 * once per chart as strict functions evaluated `with` that scope, so
 * assigning an undeclared name is a ReferenceError (error.execution) and
 * _event, _sessionid, _name, _ioprocessors and In are read-only.
 *
 * What a <script> declares at its top level (var, let, const, class,
 * function) is asked of the engine when the chart is attached (analyze). A
 * chart with such scripts is "isolated", as in the C datamodel: its scripts
 * run against a proxy of the scope that keeps implicit globals out of the
 * host's global object, and a declaring script's bindings become the
 * session's (accessors on the scope, shared with the script's functions; a
 * const stays const). Such sessions are stored like the C datamodel's
 * (docs/durability.md): variables and let/const bindings with their values,
 * top-level function declarations re-created on restore; anything else that
 * cannot be stored is refused naming the variable and, for a function, its
 * line in the document. Where one JS realm cannot be a C one: functions are
 * called with the scope as `this` (it is a `with` object), and closures other
 * than declarations keep seeing the script run that created them.
 * Charts compile with `new Function` (a CSP must allow 'unsafe-eval').
 */

const KIND = { COND: 1, VALUE: 2, LOCATION: 3, SCRIPT: 4, STRING: 5, CONTENT: 6 };
const EVENT_TYPES = ['platform', 'internal', 'external'];
const SYSVARS = new Set(['_sessionid', '_name', '_ioprocessors']);
const READONLY = new Set(['_event', '_sessionid', '_name', '_ioprocessors', 'In']);

/* ---- a minimal XML DOM for XML <content>/<data> (SCXML B.2: XML content becomes a DOM) ---- */
class XNode {
  constructor(type, name) {
    this.nodeType = type;
    this.nodeName = name;
    this.childNodes = [];
    this.parentNode = null;
  }
  appendChild(c) {
    c.parentNode = this;
    this.childNodes.push(c);
    return c;
  }
  get firstChild() {
    return this.childNodes[0] || null;
  }
  get textContent() {
    if (this.nodeType === 3 || this.nodeType === 4) return this.nodeValue;
    return this.childNodes.map((c) => c.textContent).join('');
  }
  getElementsByTagName(n) {
    const r = [];
    (function walk(e) {
      for (const c of e.childNodes)
        if (c.nodeType === 1) {
          if (n === '*' || c.nodeName === n || c.localName === n) r.push(c);
          walk(c);
        }
    })(this);
    return r;
  }
  getAttribute(n) {
    return this.attributes && Object.prototype.hasOwnProperty.call(this.attributes, n) ? this.attributes[n] : null;
  }
  hasAttribute(n) {
    return this.getAttribute(n) !== null;
  }
}
const ent = (s) =>
  s.replace(/&(lt|gt|amp|quot|apos|#x[0-9a-fA-F]+|#[0-9]+);/g, (m, e) =>
    e === 'lt' ? '<' : e === 'gt' ? '>' : e === 'amp' ? '&' : e === 'quot' ? '"' : e === 'apos' ? "'" : e[1] === 'x' ? String.fromCharCode(parseInt(e.slice(2), 16)) : String.fromCharCode(parseInt(e.slice(1), 10)),
  );
function xmlDocument(src) {
  const doc = new XNode(9, '#document');
  let cur = doc;
  const re = /<!--[\s\S]*?-->|<!\[CDATA\[([\s\S]*?)\]\]>|<\?[\s\S]*?\?>|<!DOCTYPE[^>]*>|<\/([^\s>]+)\s*>|<([^\s/>]+)([^>]*?)(\/?)>|([^<]+)/g;
  let m;
  while ((m = re.exec(src))) {
    if (m[1] !== undefined) {
      const t = new XNode(4, '#cdata-section');
      t.nodeValue = m[1];
      cur.appendChild(t);
    } else if (m[2]) {
      if (cur.parentNode) cur = cur.parentNode;
    } else if (m[3]) {
      const e = new XNode(1, m[3]);
      e.tagName = m[3];
      const k = m[3].indexOf(':');
      e.localName = k < 0 ? m[3] : m[3].slice(k + 1);
      e.attributes = {};
      const ar = /([^\s=]+)\s*=\s*("([^"]*)"|'([^']*)')/g;
      let a;
      while ((a = ar.exec(m[4]))) e.attributes[a[1]] = ent(a[3] !== undefined ? a[3] : a[4]);
      cur.appendChild(e);
      if (!m[5]) cur = e;
    } else if (m[6] !== undefined && !/^\s*$/.test(m[6])) {
      const x = new XNode(3, '#text');
      x.nodeValue = ent(m[6]);
      cur.appendChild(x);
    }
  }
  doc.documentElement = doc.childNodes.find((c) => c.nodeType === 1);
  Object.defineProperty(doc, 'toString', { value: () => src });
  return doc;
}

/* SCXML B.2: JSON if it parses, a DOM if it is XML, else a space-normalized string */
function contentValue(src) {
  try {
    return JSON.parse(src);
  } catch {
    /* not JSON */
  }
  if (/^\s*</.test(src)) {
    try {
      return xmlDocument(src);
    } catch {
      /* not XML either */
    }
  }
  return src.replace(/\s+/g, ' ').trim();
}

/* ---- what a <script> declares, asked of the engine (as chart_analyze in the C datamodel) ----
 * The source only proposes candidate identifiers. The script is compiled as a
 * function body that returns a reader per candidate before the script runs:
 * hoisting has happened, nothing executed. A reader that reaches the trap
 * scope around the function found no declaration; one that throws found a
 * let/const/class binding in its temporal dead zone; one that returns a
 * function found a function declaration (hoisted with its value); anything
 * else is a var (anywhere outside nested functions, also in blocks). */
const ID_CHAR = String.raw`[\p{ID_Continue}$\u200c\u200d]|\\u[0-9a-fA-F]{4}|\\u\{[0-9a-fA-F]+\}`;
const CANDIDATE = new RegExp(String.raw`(?:[\p{ID_Start}$_]|\\u[0-9a-fA-F]{4}|\\u\{[0-9a-fA-F]+\})(?:${ID_CHAR})*`, 'gu');
const IDENT = /^[\p{ID_Start}$_][\p{ID_Continue}$\u200c\u200d]*$/u;
const RESERVED = new Set(
  ('break case catch class const continue debugger default delete do else enum export extends false finally for function if import in ' +
    'instanceof new null return super switch this throw true try typeof var void while with yield let static implements interface package ' +
    'private protected public await arguments eval').split(' '),
);
const unescapeID = (s) => s.replace(/\\u\{([0-9a-fA-F]+)\}|\\u([0-9a-fA-F]{4})/g, (_, a, b) => String.fromCodePoint(parseInt(a || b, 16)));
const fnText = (f) => {
  try {
    return Function.prototype.toString.call(f);
  } catch {
    return '';
  }
};

/* { strict, vars, lex, fns: [{ name, src }] } of a script's top-level declarations, or null */
function analyze(src) {
  try {
    new Function(src);
  } catch {
    return null; /* it does not compile: it fails when it runs */
  }
  let strict = false;
  try {
    new Function(`${src}\n;with({});`); /* `with` is a syntax error in strict code only */
  } catch {
    strict = true;
  }
  const names = [...new Set((src.match(CANDIDATE) || []).map(unescapeID))].filter((n) => IDENT.test(n) && !RESERVED.has(n) && !n.startsWith('__ta_'));
  if (!names.length) return null;
  const missed = new Set();
  const trap = new Proxy(Object.create(null), { has: (_, k) => (missed.add(k), true), get: () => undefined });
  let readers;
  try {
    readers = new Function('__ta_t', `with(__ta_t){return function(){${strict ? "'use strict';" : ''}return [${names.map((n) => `()=>${n}`).join(',')}];\n${src}\n}}`)(trap)();
  } catch {
    return null;
  }
  const d = { strict, vars: [], lex: [], fns: [] };
  names.forEach((n, i) => {
    missed.clear();
    let v;
    try {
      v = readers[i]();
    } catch {
      d.lex.push(n); /* in its temporal dead zone */
      return;
    }
    if (missed.has(n)) return;
    if (typeof v === 'function') d.fns.push({ name: n, src: fnText(v), strict });
    else d.vars.push(n);
  });
  /* a function declared in a block (sloppy code, Annex B) is a var too, but as a
   * parameter it would not be hoisted: a real var is the one a `let` clashes with */
  d.params = d.vars.filter((n) => {
    try {
      new Function(`${src}
;let ${n};`);
      return false;
    } catch {
      return true;
    }
  });
  return d.vars.length || d.lex.length || d.fns.length ? d : null;
}

/* ---- what cannot be stored, named (as explain_var in the C datamodel) ---- */
/* what the hibernation codec (tagged JSON) keeps; class instances come back as plain objects */
const STORABLE = /^\[object (Object|Array|Date|RegExp|Map|Set|Uint8Array)\]$/;

/* [path, kind (0 function, 1 class, 2 object of another kind, 3 symbol), detail] or null */
function unstorable(v, name) {
  const seen = new Set();
  let hit = null;
  (function walk(v, p, depth) {
    if (hit || depth > 1000) return;
    if (typeof v === 'function') {
      hit = [p, /^class\b/.test(fnText(v)) ? 1 : 0, v];
      return;
    }
    if (typeof v === 'symbol') {
      hit = [p, 3, null];
      return;
    }
    if (v === null || typeof v !== 'object' || seen.has(v)) return;
    seen.add(v);
    const t = Object.prototype.toString.call(v);
    if (!STORABLE.test(t)) {
      hit = [p, 2, t.slice(8, -1)];
      return;
    }
    if (v instanceof Map) {
      let i = 0;
      for (const [k, x] of v) {
        walk(k, `${p} (key ${i})`, depth + 1);
        walk(x, `${p} (value ${i++})`, depth + 1);
      }
      return;
    }
    if (v instanceof Set) {
      let i = 0;
      for (const x of v) walk(x, `${p} (item ${i++})`, depth + 1);
      return;
    }
    if (v instanceof Date || v instanceof RegExp || v instanceof Uint8Array) return;
    const arr = Array.isArray(v);
    for (const k of Object.keys(v)) walk(v[k], arr ? `${p}[${k}]` : /^[A-Za-z_$][\w$]*$/.test(k) ? `${p}.${k}` : `${p}[${JSON.stringify(k)}]`, depth + 1);
  })(v, name, 0);
  return hit;
}

/* the document line of a function written in the chart (0: unknown); engines do not
 * report it, so its source text is looked up in the chart's expressions */
function fnLine(chart, f) {
  const text = fnText(f);
  if (!text) return 0;
  for (const x of chart.exprs) {
    const i = x && x.line ? x.source.indexOf(text) : -1;
    if (i >= 0) return x.line + (x.source.slice(0, i).match(/\n/g) || []).length;
  }
  return 0;
}

function whyNot(chart, name, v) {
  const hit = unstorable(v, name);
  if (!hit) return null;
  const [path, kind, x] = hit;
  const self = path === name;
  const line = kind < 2 ? fnLine(chart, x) : 0;
  const at = line ? ` (line ${line})` : '';
  const head = `variable '${name}' cannot be stored: `;
  if (kind === 1) return `${head}${self ? 'it' : path} is a class${at}; classes are not stored — keep the data in plain objects and the code in top-level function declarations`;
  if (kind === 0 && self) return `${head}it holds a function${at}; only top-level function declarations are stored (re-created on restore): write \`function ${name}(...) {...}\``;
  if (kind === 0) return `${head}${path} is a function${at}; functions in data are not stored`;
  if (kind === 3) return `${head}${path} is a symbol`;
  return `${head}${path} is ${/^[AEIOU]/.test(x) ? 'an' : 'a'} ${x} object`;
}

/* what was added to a declared function since its declaration (it would be lost), or null */
function fnAdded(f, name) {
  for (const k of Reflect.ownKeys(f)) if (!['length', 'name', 'prototype', 'arguments', 'caller'].includes(k)) return `${name}.${String(k)}`;
  const p = Object.getOwnPropertyDescriptor(f, 'prototype');
  if (p && p.value && typeof p.value === 'object') for (const k of Reflect.ownKeys(p.value)) if (k !== 'constructor') return `${name}.prototype.${String(k)}`;
  return null;
}

const trimExpr = (s) => s.replace(/[;\s]+$/, '');
const rootName = (s) => {
  const m = /^\s*([A-Za-z_$][A-Za-z0-9_$]*)/.exec(s);
  return m ? m[1] : null;
};
const dataProp = (o, k, v) => Object.defineProperty(o, k, { value: v, writable: true, enumerable: true, configurable: true });

export function createEcmascript() {
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const remade = new WeakMap(); /* function re-created from a declaration -> its name */

  /* chart: { exprs: [{ source, kind, line }], data: [{ id, expr }], decls[e] (scripts that
   * declare globals), isolated, key, get[], set[], run[], make } */
  function attach(info) {
    const chart = { name: info.name, exprs: info.exprs, data: info.data, get: [], set: [], run: [], decls: [], fdecl: new Map(), make: new Map() };
    let hash = 0x811c9dc5;
    info.exprs.forEach((x, e) => {
      if (!x || x.kind !== KIND.SCRIPT) return;
      for (let i = 0; i < x.source.length; i++) hash = Math.imul(hash ^ x.source.charCodeAt(i), 16777619);
      const d = analyze(x.source);
      if (!d) return;
      chart.decls[e] = d;
      for (const f of d.fns) chart.fdecl.set(f.name, Object.assign(f, { e }));
    });
    chart.isolated = chart.decls.some(Boolean);
    /* which declarations a stored session refers to: blobs for another version re-run its own */
    chart.key = `${info.name}#${(hash >>> 0).toString(36)}`;
    return chart;
  }

  function getter(chart, e) {
    let f = chart.get[e];
    if (f) return f;
    const x = chart.exprs[e];
    if (!x) throw new RangeError('bad expression id');
    if (x.kind === KIND.SCRIPT) f = new Function('__ta_s', `with(__ta_s){(function(){'use strict';\n${x.source}\n})();}`);
    else f = new Function('__ta_s', `with(__ta_s){return (function(){'use strict';return (\n${trimExpr(x.source)}\n);})();}`);
    return (chart.get[e] = f);
  }

  /* A script that declares globals runs as a function body (strict if the script is)
   * whose var names are parameters, so re-declaring keeps a value as a global var does.
   * Before the script, __ta_x (hoisted) and the hoisted functions are handed out: even
   * when it throws, its bindings become the session's — accessors on the scope that
   * read and write them, so the session and the script's functions share them. */
  function runner(chart, e) {
    let r = chart.run[e];
    if (r) return r;
    const d = chart.decls[e];
    const params = d.params.filter((n) => !READONLY.has(n));
    const names = [...d.vars, ...d.fns.map((f) => f.name), ...d.lex];
    const cases = names.map((n, i) => `case ${i}:if(__ta_w){${n}=__ta_v;return}return ${n};`).join('');
    const f = new Function(
      '__ta_s',
      '__ta_o',
      '__ta_a',
      `with(__ta_s){return (function(${params.join(',')}){${d.strict ? "'use strict';" : ''}__ta_o.x=__ta_x;__ta_o.h=[${d.fns.map((f) => f.name).join(',')}];\n` +
        `${chart.exprs[e].source}\n;function __ta_x(__ta_k,__ta_w,__ta_v){switch(__ta_k){${cases}}}}).apply(void 0,__ta_a);}`,
    );
    return (chart.run[e] = { f, params, names, d, e });
  }

  function declare(inst, r) {
    const decl = inst.decl;
    for (const n of r.d.lex) if (decl.has(n)) throw new SyntaxError(`Identifier '${n}' has already been declared`);
    for (const n of r.names) if (decl.has(n) && decl.get(n).lex) throw new SyntaxError(`Identifier '${n}' has already been declared`);
    const out = {};
    try {
      return r.f(proxyOf(inst), out, r.params.map((n) => inst.scope[n]));
    } finally {
      if (out.x) bind(inst, r, out);
    }
  }

  function bind(inst, r, out) {
    const x = out.x, nv = r.d.vars.length, nf = r.d.fns.length;
    r.names.forEach((n, i) => {
      if (READONLY.has(n)) return;
      let kind = i < nv ? 'var' : i < nv + nf ? 'fn' : 'let';
      if (kind === 'let')
        try {
          x(i, true, x(i));
        } catch (err) {
          if (err instanceof TypeError) kind = 'const'; /* (a ReferenceError: never initialized) */
        }
      inst.decl.set(n, { kind, lex: kind === 'let' || kind === 'const', e: r.e });
      Object.defineProperty(inst.scope, n, { get: () => x(i), set: (v) => x(i, true, v), enumerable: true, configurable: true });
    });
    r.d.fns.forEach((f, j) => inst.orig.set(f.name, out.h[j]));
  }

  /* a function declaration again, closing over the session (restore) */
  function remake(inst, fd) {
    const key = `${fd.e}:${fd.name}`;
    let mk = inst.chart.make.get(key);
    if (!mk) inst.chart.make.set(key, (mk = new Function('__ta_s', `with(__ta_s){return (function(){${fd.strict ? "'use strict';" : ''}return (\n${fd.src}\n);})();}`)));
    const fn = mk(proxyOf(inst));
    remade.set(fn, fd.name);
    return fn;
  }

  function setter(chart, e) {
    let f = chart.set[e];
    if (f) return f;
    const x = chart.exprs[e];
    if (!x) throw new RangeError('bad expression id');
    f = new Function('__ta_s', '__ta_v', `with(__ta_s){(\n${x.source}\n) = __ta_v;}`);
    return (chart.set[e] = f);
  }

  /* the scope an isolated chart's scripts run against: unknown names (not host
   * globals) resolve to the session, so `x = 1` declares x there */
  function proxyOf(inst) {
    if (!inst.proxy)
      inst.proxy = new Proxy(inst.scope, {
        /* (not the wrapper's own __ta_ names) */
        has: (t, k) => typeof k === 'string' && (k in t || (!(k in globalThis) && !k.startsWith('__ta_'))),
      });
    return inst.proxy;
  }

  function value(inst, e) {
    const x = inst.chart.exprs[e];
    if (x && x.kind === KIND.CONTENT) return contentValue(x.source);
    if (x && x.kind === KIND.SCRIPT && inst.chart.decls[e]) return declare(inst, runner(inst.chart, e));
    const f = getter(inst.chart, e);
    return f(inst.chart.isolated && x.kind === KIND.SCRIPT ? proxyOf(inst) : inst.scope);
  }

  /* a location must start with one of the session's own variables */
  function locationOk(inst, e) {
    const name = rootName(inst.chart.exprs[e].source);
    return !!name && !READONLY.has(name) && own(inst.scope, name);
  }

  function assignTo(inst, loc, v) {
    if (!locationOk(inst, loc)) throw new ReferenceError('not a declared location');
    const src = inst.chart.exprs[loc].source.trim();
    const d = Object.getOwnPropertyDescriptor(inst.scope, src);
    if (d && 'value' in d && !d.writable) throw new TypeError(`Assignment to constant variable '${src}'`);
    setter(inst.chart, loc)(inst.scope, v);
  }

  const ro = (o, k, v) => Object.defineProperty(o, k, { value: v, enumerable: true, configurable: true, writable: false });

  function create(chart, sys, init, In) {
    const scope = Object.create(null);
    ro(scope, 'In', In);
    ro(scope, '_sessionid', String(sys.sessionid));
    ro(scope, '_name', String(sys.name || ''));
    const io = {};
    for (const p of sys.ioprocessors || []) {
      const e = { location: p.location };
      io[p.type] = e;
      if (p.type.endsWith('#SCXMLEventProcessor')) io.scxml = e;
      if (p.type.endsWith('#BasicHTTPEventProcessor')) io.basichttp = e;
    }
    ro(scope, '_ioprocessors', io);
    /* declared from the start (late binding: undefined); a migration may drop them */
    for (const d of chart.data) dataProp(scope, d.id, undefined);
    /* decl: names scripts declared -> { kind: var|fn|let|const, lex, e }; orig: the
     * functions the last run of a script declared */
    return { chart, scope, init, proxy: null, sys, decl: new Map(), orig: new Map() };
  }

  /* each call: true on success, false = error.execution */
  const run = (fn) => {
    try {
      return fn();
    } catch {
      return false;
    }
  };

  return {
    KIND,
    attach,
    create,
    initData(inst, decl) {
      const d = inst.chart.data[decl];
      if (!d) return false;
      if (inst.init && typeof inst.init === 'object' && d.id in inst.init) {
        inst.scope[d.id] = inst.init[d.id]; /* <invoke> namelist/param: overrides the expr */
        return true;
      }
      if (d.expr < 0) return true;
      try {
        inst.scope[d.id] = value(inst, d.expr);
        return true;
      } catch {
        inst.scope[d.id] = undefined;
        return false;
      }
    },
    bindEvent(inst, ev) {
      const o = {
        name: ev.name,
        type: EVENT_TYPES[ev.type % 3],
        sendid: ev.sendid || undefined,
        origin: ev.origin || undefined,
        origintype: ev.origintype || undefined,
        invokeid: ev.invokeid || undefined,
        data: ev.data,
      };
      if (ev.raw) o.raw = ev.raw;
      ro(inst.scope, '_event', o);
    },
    cond(inst, e) {
      try {
        return value(inst, e) ? 1 : 0;
      } catch {
        return -1;
      }
    },
    /* *expr attributes; null = error.execution */
    string(inst, e) {
      try {
        return String(value(inst, e));
      } catch {
        return null;
      }
    },
    log(inst, e) {
      let v;
      try {
        v = value(inst, e);
      } catch {
        return null;
      }
      if (v && typeof v === 'object') {
        try {
          return JSON.stringify(v);
        } catch {
          /* cycles: as a string */
        }
      }
      try {
        return String(v);
      } catch {
        return null;
      }
    },
    assign: (inst, loc, val) => run(() => (assignTo(inst, loc, value(inst, val)), true)),
    assignString: (inst, loc, s) => run(() => (assignTo(inst, loc, s), true)),
    script: (inst, e) => run(() => (value(inst, e), true)),
    /* the items (a shallow copy), or null = error.execution */
    foreachBegin(inst, arr) {
      try {
        const a = value(inst, arr);
        return Array.isArray(a) ? a.slice() : null;
      } catch {
        return null;
      }
    },
    foreachItem(inst, items, i, item, index) {
      return run(() => {
        for (const loc of index >= 0 ? [item, index] : [item]) {
          const s = inst.chart.exprs[loc].source.trim(); /* <foreach> may introduce item/index */
          if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(s) && !own(inst.scope, s) && !READONLY.has(s)) dataProp(inst.scope, s, undefined);
        }
        assignTo(inst, item, items[i]);
        if (index >= 0) assignTo(inst, index, i);
        return true;
      });
    },
    /* <send>/<invoke>/<donedata> data: { ok, v } */
    capture(inst, content, namelist, params) {
      try {
        if (content >= 0) return { ok: true, v: value(inst, content) };
        const v = {};
        for (const e of namelist) v[inst.chart.exprs[e].source.trim()] = value(inst, e);
        for (const [name, e] of params) v[name] = value(inst, e);
        return { ok: true, v };
      } catch {
        return { ok: false };
      }
    },
    /* The session as plain values (docs/durability.md): its variables and _event
     * (system variables come back on restore); under keys no data id can be: the
     * <invoke> params late binding still applies ('#init'), let/const/class bindings
     * ('#lex': [name, 0 let | 1 const | 2 uninitialized, value]), top-level function
     * declarations, re-created on restore ('#fns': [name, script]), and which
     * declarations those are ('#chart'). What cannot be stored throws, naming the
     * variable (and a function's line). */
    serialize(inst) {
      const o = {};
      const lex = [], fns = [];
      for (const k of Object.keys(inst.scope)) {
        if (SYSVARS.has(k) || k === 'In') continue;
        const dk = inst.decl.get(k);
        let v;
        try {
          v = inst.scope[k];
        } catch {
          lex.push([k, 2]); /* still uninitialized */
          continue;
        }
        if (typeof v === 'function' && dk && dk.kind === 'fn' && (remade.get(v) === k || inst.orig.get(k) === v)) {
          const added = fnAdded(v, k);
          if (added) throw new Error(`function '${k}' cannot be stored: ${added} was added after its declaration (only unchanged top-level function declarations are re-created)`);
          fns.push([k, dk.e]);
          continue;
        }
        const why = whyNot(inst.chart, k, v);
        if (why) throw new Error(why);
        if (dk && dk.lex) lex.push([k, dk.kind === 'const' ? 1 : 0, v]);
        else o[k] = v;
      }
      if (inst.init !== undefined) o['#init'] = inst.init;
      if (lex.length) o['#lex'] = lex;
      if (fns.length) o['#fns'] = fns;
      if (inst.chart.isolated) o['#chart'] = inst.chart.key;
      return o;
    },
    /* a blob of another version of the chart (a migration): lexical bindings come back
     * as plain variables (the migration drops what the new chart does not declare) and
     * functions from the new chart's declarations of their names */
    deserialize(chart, sys, o, In) {
      o = o || {};
      const inst = create(chart, sys, o['#init'], In);
      const scope = inst.scope;
      const same = !o['#chart'] || o['#chart'] === chart.key;
      /* <data> of this version the stored session did not have: not previous bindings */
      inst.added = chart.data.map((d) => d.id).filter((k) => !own(o, k));
      for (const [k, v] of Object.entries(o)) {
        if (k[0] === '#') continue;
        if (k === '_event') ro(scope, k, v);
        else dataProp(scope, k, v);
      }
      for (const [k, kind, v] of o['#lex'] || []) {
        if (!same) {
          if (kind !== 2) dataProp(scope, k, v);
          continue;
        }
        inst.decl.set(k, { kind: kind === 1 ? 'const' : 'let', lex: true });
        if (kind === 2) {
          const tdz = () => {
            throw new ReferenceError(`Cannot access '${k}' before initialization`);
          };
          Object.defineProperty(scope, k, { get: tdz, set: tdz, enumerable: true, configurable: true });
        } else Object.defineProperty(scope, k, { value: v, writable: kind === 0, enumerable: true, configurable: true });
      }
      for (const [k, e] of o['#fns'] || []) {
        const fd = (same && chart.decls[e] && chart.decls[e].fns.find((f) => f.name === k)) || chart.fdecl.get(k);
        if (!fd) continue; /* not declared by this version */
        inst.decl.set(k, { kind: 'fn', lex: false, e: fd.e });
        dataProp(scope, k, remake(inst, fd));
      }
      return inst;
    },
    /* live migration: the previous bindings (functions are code the new chart
     * re-creates, not bindings); variables the new chart declares nowhere go */
    migrate(inst) {
      const prev = {};
      const declared = new Set(inst.chart.data.map((d) => d.id));
      for (const d of inst.chart.decls) if (d) for (const n of [...d.vars, ...d.lex, ...d.fns.map((f) => f.name)]) declared.add(n);
      for (const k of Object.keys(inst.scope)) {
        if (SYSVARS.has(k) || k === '_event' || k === 'In') continue;
        try {
          const v = inst.scope[k];
          if (!(typeof v === 'function' && remade.has(v)) && !(inst.added && inst.added.includes(k))) prev[k] = v;
        } catch {
          /* uninitialized */
        }
        if (!declared.has(k)) {
          delete inst.scope[k];
          inst.decl.delete(k);
        }
      }
      return prev;
    },
    /* the variables, for snapshots (getSnapshot().context) */
    vars(inst) {
      const o = {};
      for (const k of Object.keys(inst.scope)) {
        if (SYSVARS.has(k) || k === '_event' || k === 'In') continue;
        try {
          o[k] = inst.scope[k];
        } catch {
          /* uninitialized */
        }
      }
      return o;
    },
  };
}
