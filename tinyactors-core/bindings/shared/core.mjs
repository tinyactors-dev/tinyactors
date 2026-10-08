/**
 * tinyactors — the binding-independent core of the JavaScript API (shared by
 * the Node.js N-API binding and the wasm/browser binding).
 *
 * An xstate-style API on top of the tinyactors SCXML actor runtime. Machines
 * compile to readable SCXML: guards and actions appear by name
 * (`cond="isAdult"`, `<script>track({"tag":"t"})</script>`), inline functions
 * by their source, literals as JSON. Each machine keeps a registry from that
 * text to the JavaScript function implementing it; when a chart is attached
 * to a system the datamodel resolves every expression of the chart once, so
 * evaluation is an indexed call — the index never appears in the chart.
 *
 * All actors of a `System` live in one native actor system. Machines are
 * linked into the system image when the first actor starts; machines defined
 * after that are linked into an additional segment, and segments talk
 * SCXML-to-SCXML through the runtime's remote I/O processor.
 *
 * `native` is the procedural interface of a backend (addon.c for Node,
 * web/native.mjs over the wasm module for browsers); `platform` supplies the
 * clock, timers, deferral and logging.
 */

import { createEcmascript } from './ecmascript.mjs';

export function createRuntime({ native, platform }) {
  /* fuel per slice; yielded work continues on platform.defer. The budget adapts
   * so a slice takes about SLICE_MS of wall time (backends differ in speed; in
   * browsers a slice must stay well inside a frame). */
  const NONE = Object.freeze([]);
  const BUDGET = 10000;
  const SLICE_MS = 4;

  /* ---------------------------------------------------------------- creators */

  function assign(assignment) {
    return { type: 'xstate.assign', assignment };
  }
  function raise(event, options = {}) {
    return { type: 'xstate.raise', event, delay: options.delay, id: options.id };
  }
  function sendTo(to, event, options = {}) {
    return { type: 'xstate.sendTo', to, event, delay: options.delay, id: options.id };
  }
  function sendParent(event, options = {}) {
    return { type: 'xstate.sendParent', event, delay: options.delay, id: options.id };
  }
  function log(value = ({ context, event }) => ({ context, event }), label) {
    return { type: 'xstate.log', value, label };
  }
  function cancel(sendID) {
    return { type: 'xstate.cancel', sendID };
  }
  function stateIn(stateValue) {
    return { type: 'xstate.stateIn', stateValue };
  }
  function and(guards) {
    return { type: 'xstate.and', guards };
  }
  function or(guards) {
    return { type: 'xstate.or', guards };
  }
  function not(guard) {
    return { type: 'xstate.not', guards: [guard] };
  }

  /* ------------------------------------------------------------- XML output */

  const toArray = (x) => (x === undefined ? [] : Array.isArray(x) ? x : [x]);
  const norm = (s) => String(s).replace(/\s+/g, ' ').trim();
  const escText = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');

  function attrText(v) {
    const s = String(v);
    if (s.includes('"') && !s.includes("'")) return `'${escText(s)}'`;
    return `"${escText(s).replace(/"/g, '&quot;')}"`;
  }

  /** el('state', { id: 'a' }, ...children) — attributes with undefined values are dropped */
  function el(tag, attrs, ...children) {
    return { tag, attrs: attrs || {}, children: children.flat(Infinity).filter(Boolean) };
  }

  function serialize(node, indent = '') {
    if (typeof node === 'string') return indent + escText(node);
    const attrs = Object.entries(node.attrs)
      .filter(([, v]) => v !== undefined)
      .map(([k, v]) => ` ${k}=${attrText(v)}`)
      .join('');
    const open = `${indent}<${node.tag}${attrs}`;
    if (!node.children.length) return `${open}/>`;
    if (node.children.length === 1 && typeof node.children[0] === 'string')
      return `${open}>${escText(node.children[0])}</${node.tag}>`;
    const inner = node.children.map((c) => serialize(c, indent + '  ')).join('\n');
    return `${open}>\n${inner}\n${indent}</${node.tag}>`;
  }

  /* ---------------------------------------------------------------- compiler */

  const fnIDs = new WeakMap();
  let fnSeq = 0;
  function fnKey(f) {
    if (!fnIDs.has(f)) fnIDs.set(f, `f${++fnSeq}`);
    return fnIDs.get(f);
  }
  const json = (v) => (v === undefined ? 'undefined' : JSON.stringify(v));
  const clone = (v) => (v !== null && typeof v === 'object' ? structuredClone(v) : v);

  /** A readable expression: source text, implementation, identity. */
  const X = (text, fn, key) => ({ text, fn, key });
  const literal = (v) => X(json(v), () => clone(v), `lit:${json(v)}`);
  const userFn = (f, params) =>
    X(f.toString(), (args) => f(args, params), params === undefined ? fnKey(f) : `${fnKey(f)}(${json(params)})`);
  const valueOf = (v) => (typeof v === 'function' ? userFn(v) : literal(v));

  /* --------------------------------------------- per-property decomposition
   * A context (`{ a: 1 }` or `({ input }) => ({ a: input.a, b: [] })`) becomes
   * one <data> per property, an assignment one <assign> per property. Function
   * sources are split at the top-level commas of a returned object literal;
   * anything else (block bodies, spreads, computed keys, methods) is opaque. */

  /** Index just past the bracket/string/comment/regex starting at s[i]. */
  function skip(s, i) {
    const c = s[i];
    if (c === '"' || c === "'") {
      for (i++; i < s.length && s[i] !== c; i++) if (s[i] === '\\') i++;
      return i + 1;
    }
    if (c === '`') {
      for (i++; i < s.length && s[i] !== '`'; i++) {
        if (s[i] === '\\') i++;
        else if (s[i] === '$' && s[i + 1] === '{') i = skip(s, i + 1) - 1;
      }
      return i + 1;
    }
    if (c === '/' && s[i + 1] === '/') return s.indexOf('\n', i) < 0 ? s.length : s.indexOf('\n', i);
    if (c === '/' && s[i + 1] === '*') return s.indexOf('*/', i) < 0 ? s.length : s.indexOf('*/', i) + 2;
    if (c === '/') {
      /* a regex literal (only where an operand may start), else division */
      let j = i - 1;
      while (j >= 0 && /\s/.test(s[j])) j--;
      if (j >= 0 && !/[(,=:[!&|?{};+\-*%<>~^]/.test(s[j])) return i + 1;
      for (i++; i < s.length && s[i] !== '/'; i++) {
        if (s[i] === '\\') i++;
        else if (s[i] === '[') while (++i < s.length && s[i] !== ']') if (s[i] === '\\') i++;
      }
      return i + 1;
    }
    const close = { '(': ')', '[': ']', '{': '}' }[c];
    if (!close) return i + 1;
    for (i++; i < s.length && s[i] !== close; ) i = skip(s, i);
    return i + 1;
  }

  /** [{ key, text }] for the properties of an object literal source `{ ... }`, or null. */
  function objectProps(src) {
    src = src.trim();
    if (src[0] !== '{' || skip(src, 0) !== src.length || src[src.length - 1] !== '}') return null;
    const props = [];
    const body = src.slice(1, -1);
    let start = 0;
    for (let i = 0; i <= body.length; ) {
      if (i === body.length || body[i] === ',') {
        const part = body.slice(start, i).replace(/^(\s|\/\*[\s\S]*?\*\/|\/\/[^\n]*\n)+/, '').trim();
        if (part) {
          const m = /^(?:([A-Za-z_$][\w$]*)|"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)')\s*(:|$)/.exec(part);
          if (!m || (m[4] !== ':' && m[1] === undefined)) return null; // spread, computed key, method
          const key = m[1] !== undefined ? m[1] : m[2] !== undefined ? m[2] : m[3];
          let text = m[4] === ':' ? part.slice(m[0].length) : key;
          text = text.replace(/(\s|\/\*[\s\S]*?\*\/|\/\/[^\n]*\n)+$/, '').trim();
          if (!text) return null;
          props.push({ key, text: norm(text) });
        }
        start = ++i;
      } else i = skip(body, i);
    }
    return props;
  }

  /** The returned object literal's properties of an expression-bodied arrow function, or null. */
  function returnedProps(f) {
    const src = f.toString().trim();
    let i = 0;
    if (src[0] === '(') i = skip(src, 0);
    else {
      const m = /^[A-Za-z_$][\w$]*/.exec(src);
      if (!m) return null;
      i = m[0].length;
    }
    const rest = src.slice(i).trimStart();
    if (!rest.startsWith('=>')) return null;
    const body = rest.slice(2).trim();
    if (body[0] !== '(' || skip(body, 0) !== body.length) return null;
    return objectProps(body.slice(1, -1));
  }

  /** The body of an expression-bodied arrow `({ ... }) => expr`, for display. */
  function bodyText(f) {
    const src = f.toString().trim();
    if (src[0] !== '(') return null;
    const rest = src.slice(skip(src, 0)).trimStart();
    if (!rest.startsWith('=>')) return null;
    const body = rest.slice(2).trim();
    if (body[0] === '{') return null;
    return norm(body[0] === '(' && skip(body, 0) === body.length ? body.slice(1, -1) : body);
  }

  /** A JS literal written as a property value (`0`, `null`, `[]`, `''`, `"x"`), as { v } or null. */
  function literalText(text) {
    if (text === 'undefined') return { v: undefined };
    const q = /^'((?:[^'\\"]|\\.)*)'$/.exec(text);
    try {
      return { v: JSON.parse(q ? `"${q[1]}"` : text) };
    } catch {
      return null;
    }
  }

  /* Picks of one computation: the first evaluates `compute` and keeps the
   * result for the others (keyed by the evaluating actor), so every property
   * sees the same inputs — xstate computes all of an assignment's properties
   * from the previous context. Groups are evaluated contiguously by one actor. */
  const pending = new Map();
  function picks(compute, entries, key) {
    const n = entries.length;
    return entries.map(({ key: k, text }, i) => {
      let fn;
      if (n === 1) fn = (args, $) => objectOf(compute(args, $))[k];
      else if (i === 0)
        fn = (args, $) => {
          const r = objectOf(compute(args, $));
          pending.set(`${$.self}|${key}`, r);
          return r[k];
        };
      else
        fn = (args, $) => {
          const m = `${$.self}|${key}`;
          const r = pending.has(m) ? pending.get(m) : objectOf(compute(args, $));
          if (i === n - 1) pending.delete(m);
          return r[k];
        };
      return { k, x: X(text, fn, `pick:${key}:${i}/${n}`) };
    });
  }
  function objectOf(r) {
    if (r === null || typeof r !== 'object') throw new TypeError('expected an object of context properties');
    return r;
  }

  class Compiler {
    constructor() {
      this.registry = new Map(); /* normalized text -> { fn, key } */
      this.invokeSeq = 0;
    }

    /** Register an expression; returns the text to write into the chart. */
    expr(x) {
      const base = norm(x.text);
      for (let n = 1, text = base; ; text = `${base} /* ${++n} */`) {
        const e = this.registry.get(text);
        if (!e) {
          this.registry.set(text, { fn: x.fn, key: x.key });
          return text;
        }
        if (e.key === x.key) return text; // same implementation: share the expression
      }
    }

    /** Compile a machine (and, recursively, invoked child machines) to an SCXML element tree. */
    machine(machine) {
      const config = machine.config;
      const chart = { impl: machine.implementations, nodes: new Map(), ids: new Map(), byID: new Map(), doneID: null };
      if (!this.rootChart) this.rootChart = chart; // the model of the outermost machine (state values, tags)
      const root = this.collect(chart, config, '', null);
      root.id = machine.id;
      const machineLevel = ['on', 'entry', 'exit', 'always', 'after', 'invoke'].some((k) => config[k] !== undefined);
      const wrap = machineLevel || root.type === 'parallel' || !root.children.length;
      if (wrap) {
        let id = machine.id;
        while (chart.nodes.has(id)) id = `${id}_`;
        root.id = id;
      }
      chart.byID.set(root.id, root);
      chart.root = root;
      chart.wrapped = wrap;

      const finals = root.children.filter((c) => c.type === 'final');
      const others = root.children.filter((c) => c.type !== 'final');
      const body = [];
      const output = config.output;
      const doneID = `${root.id}.done`;
      if (wrap && root.type === 'parallel') {
        // a parallel machine is done when all regions are: leave through a top-level final
        body.push(this.state(chart, root, [el('transition', { event: `done.state.${root.id}`, target: doneID })]));
        chart.doneID = doneID;
        body.push(el('final', { id: doneID }, output !== undefined && this.donedata(valueOf(output))));
      } else {
        if (wrap) body.push(this.state(chart, root, [], others));
        else body.push(...others.map((k) => this.state(chart, k)));
        for (const f of finals) body.push(this.state(chart, f, [], undefined, output));
      }
      const ctx = config.context;
      const initialNode = wrap ? root : this.initialChild(root);
      return el(
        'scxml',
        {
          xmlns: 'http://www.w3.org/2005/07/scxml',
          version: '1.0',
          datamodel: 'xstate',
          name: machine.id,
          initial: initialNode.id,
        },
        ctx !== undefined && el('datamodel', null, this.contextData(ctx)),
        body,
      );
    }

    /** <data> per context property; an opaque factory stays one <data id="context">. */
    contextData(ctx) {
      const plain = ctx !== null && typeof ctx === 'object' && !Array.isArray(ctx);
      const props = typeof ctx === 'function' ? returnedProps(ctx) : null;
      if (plain && !('context' in ctx))
        return Object.entries(ctx).map(([k, v]) => el('data', { id: k, expr: this.expr(literal(v)) }));
      if (!props || props.some((p) => p.key === 'context'))
        return el('data', { id: 'context', expr: this.expr(valueOf(ctx)) });
      // the factory runs once per instance; each <data> shows its property's source
      const computed = props.filter((p) => !literalText(p.text));
      const x = new Map(picks((args) => ctx(args), computed, `ctx:${fnKey(ctx)}`).map((p) => [p.k, p.x]));
      return props.map((p) => {
        const lit = literalText(p.text);
        const v = lit && lit.v;
        return el('data', { id: p.key, expr: this.expr(lit ? X(p.text, () => clone(v), `lit:${json(v)}`) : x.get(p.key)) });
      });
    }

    collect(chart, config, path, parent) {
      const node = {
        key: path.split('.').pop(),
        path,
        id: path,
        parent,
        config,
        children: [],
        tags: toArray(config.tags),
        type: config.type || (config.states && Object.keys(config.states).length ? 'compound' : 'atomic'),
      };
      if (parent) {
        chart.nodes.set(path, node);
        chart.byID.set(path, node);
        if (config.id) chart.ids.set(config.id, path);
      }
      for (const [k, c] of Object.entries(config.states || {}))
        node.children.push(this.collect(chart, c, parent ? `${path}.${k}` : k, node));
      return node;
    }

    initialChild(node) {
      const init = node.config.initial !== undefined ? node.config.initial : node.children[0].key;
      const child = node.children.find((k) => k.key === init);
      if (!child) throw new Error(`initial state "${init}" not found in "${node.path || '(machine)'}"`);
      return child;
    }

    resolveTarget(chart, node, target) {
      if (target.startsWith('#')) {
        const p = chart.ids.get(target.slice(1));
        if (p === undefined) throw new Error(`unknown state id ${target}`);
        return p;
      }
      const base = target.startsWith('.') ? node : node.parent || node;
      const rel = target.startsWith('.') ? target.slice(1) : target;
      const p = base.parent ? `${base.path}.${rel}` : rel;
      if (!chart.nodes.has(p)) throw new Error(`unknown target "${target}" from "${node.path || '(machine)'}"`);
      return p;
    }

    /* guards -> readable expressions */
    guardExpr(chart, g, params) {
      if (typeof g === 'function') return userFn(g, params);
      if (typeof g === 'string') return this.guardExpr(chart, { type: g });
      switch (g.type) {
        case 'xstate.stateIn': {
          let p = g.stateValue;
          if (typeof p !== 'string') throw new Error('stateIn() expects a state path string');
          if (p.startsWith('#')) p = this.resolveTarget(chart, chart.root, p);
          return X(`In('${p}')`, (args, $) => $.in(p), `in:${p}`);
        }
        case 'xstate.and':
        case 'xstate.or':
        case 'xstate.not': {
          const subs = g.guards.map((s) => this.guardExpr(chart, s));
          const name = g.type.slice(7);
          const fns = subs.map((s) => s.fn);
          const fn =
            name === 'and'
              ? (args, $) => fns.every((f) => f(args, $))
              : name === 'or'
                ? (args, $) => fns.some((f) => f(args, $))
                : (args, $) => !fns[0](args, $);
          return X(`${name}(${subs.map((s) => s.text).join(', ')})`, fn, `${name}(${subs.map((s) => s.key).join(',')})`);
        }
        default: {
          const named = chart.impl.guards && chart.impl.guards[g.type];
          if (named === undefined) throw new Error(`unknown guard "${g.type}"`);
          const p = g.params;
          if (typeof named !== 'function') return this.guardExpr(chart, named, p);
          const text = p === undefined ? g.type : `${g.type}(${json(p)})`;
          return X(text, (args) => named(args, p), `guard:${text}`);
        }
      }
    }

    delayAttrs(chart, delay) {
      if (delay === undefined) return {};
      if (typeof delay === 'number') return { delay: `${delay}ms` };
      if (typeof delay === 'function') return { delayexpr: this.expr(userFn(delay)) };
      const named = chart.impl.delays && chart.impl.delays[delay];
      if (named === undefined) throw new Error(`unknown delay "${delay}"`);
      const fn = typeof named === 'function' ? (args) => named(args) : () => named;
      return { delayexpr: this.expr(X(delay, fn, `delay:${delay}`)) };
    }

    donedata(x) {
      return el('donedata', null, el('content', { expr: this.expr(x) }));
    }

    /* actions -> executable content */
    actions(chart, list) {
      return toArray(list).map((a) => this.action(chart, a));
    }

    action(chart, a, params) {
      if (typeof a === 'function') return el('script', null, this.expr(userFn(a, params)));
      if (typeof a === 'string') return this.action(chart, { type: a });
      switch (a.type) {
        case 'xstate.assign':
          return this.assignAction(a.assignment, params);
        case 'xstate.raise':
        case 'xstate.sendTo':
        case 'xstate.sendParent':
          return this.sendAction(chart, a);
        case 'xstate.log':
          return el('log', { label: a.label, expr: this.expr(valueOf(a.value)) });
        case 'xstate.cancel':
          return el('cancel', { sendid: a.sendID });
        default: {
          const named = chart.impl.actions && chart.impl.actions[a.type];
          if (named === undefined) throw new Error(`unknown action "${a.type}"`);
          if (typeof named !== 'function') return this.action(chart, named, a.params);
          const p = a.params;
          const text = p === undefined ? a.type : `${a.type}(${json(p)})`;
          return el('script', null, this.expr(X(text, (args) => named(args, p), `action:${text}`)));
        }
      }
    }

    /* assign() -> one <assign location="prop"> per property. Computed
     * properties come first and are evaluated together against the previous
     * context (xstate semantics); constants follow. */
    assignAction(spec, params) {
      const one = (k, x) => el('assign', { location: k, expr: this.expr(x) });
      const pkey = params === undefined ? '' : json(params);
      if (typeof spec === 'function') {
        const props = returnedProps(spec);
        if (!props || !props.length) return el('assign', { location: 'context', expr: this.expr(userFn(spec, params)) });
        const computed = props.filter((p) => !literalText(p.text));
        const consts = props.filter((p) => literalText(p.text));
        return [
          picks((args) => spec(args, params), computed, `as:${fnKey(spec)}${pkey}`).map((p) => one(p.k, p.x)),
          consts.map((p) => {
            const { v } = literalText(p.text);
            return one(p.key, X(p.text, () => clone(v), `lit:${json(v)}`));
          }),
        ];
      }
      const keys = Object.keys(spec);
      const fns = keys.filter((k) => typeof spec[k] === 'function');
      const consts = keys.filter((k) => typeof spec[k] !== 'function');
      const shown = (f) => bodyText(f) || f.toString();
      let computed;
      if (fns.length === 1) {
        const f = spec[fns[0]];
        computed = [{ k: fns[0], x: X(shown(f), (args) => f(args, params), params === undefined ? fnKey(f) : `${fnKey(f)}(${pkey})`) }];
      } else {
        const all = (args) => Object.fromEntries(fns.map((k) => [k, spec[k](args, params)]));
        const key = `as:{${fns.map((k) => `${k}=${fnKey(spec[k])}`).join(',')}}${pkey}`;
        computed = picks(all, fns.map((k) => ({ key: k, text: shown(spec[k]) })), key);
      }
      return [computed.map((p) => one(p.k, p.x)), consts.map((k) => one(k, literal(spec[k])))];
    }

    sendAction(chart, a) {
      const ev = a.event;
      const attrs = {};
      let content = null;
      if (typeof ev === 'function') {
        const text = this.expr(userFn(ev));
        attrs.eventexpr = text; // an event object stands for its type
        content = el('content', { expr: text });
      } else {
        const obj = typeof ev === 'string' ? { type: ev } : ev;
        attrs.event = obj.type;
        if (Object.keys(obj).length > 1) content = el('content', { expr: this.expr(literal(obj)) });
      }
      if (a.type === 'xstate.raise') {
        if (a.delay === undefined && a.id === undefined && !content) return el('raise', { event: attrs.event });
        if (a.delay === undefined) attrs.target = '#_internal';
      } else if (a.type === 'xstate.sendParent') {
        attrs.target = '#_parent';
      } else if (typeof a.to === 'string') {
        attrs.target = `#_${a.to}`;
      } else if (typeof a.to === 'function') {
        const f = a.to;
        attrs.targetexpr = this.expr(X(f.toString(), (args) => addressOf(f(args)), `to:${fnKey(f)}`));
      } else if (a.to instanceof ActorRef) {
        const ref = a.to;
        attrs.targetexpr = this.expr(X(`ref(${json(ref.address)})`, () => ref.address, `to:ref:${ref.address}`));
      } else if (a.to instanceof Actor) {
        const ref = a.to;
        const name = ref.options.systemID || ref.id;
        attrs.targetexpr = this.expr(X(`system.get(${json(name)})`, () => addressOf(ref), `to:actor:${name}`));
      } else {
        throw new Error('sendTo() expects a child id, an actor, or a function returning one');
      }
      Object.assign(attrs, this.delayAttrs(chart, a.delay));
      if (a.id !== undefined) attrs.id = a.id;
      return el('send', attrs, content);
    }

    transitions(chart, node, event, spec) {
      return toArray(spec).map((t) => {
        if (typeof t === 'string') t = { target: t };
        let targets = toArray(t.target).map((x) => this.resolveTarget(chart, node, x));
        // xstate v5: transitions to the source itself do not re-enter by default
        if (!t.reenter && targets.length === 1 && targets[0] === node.path) targets = [];
        // targets inside the source do not exit it (SCXML type="internal"); for the
        // machine root that is everything except its finals, which live outside it
        const inside =
          targets.length &&
          targets.every((p) =>
            node === chart.root
              ? !(chart.nodes.get(p).parent === node && chart.nodes.get(p).type === 'final')
              : p.startsWith(`${node.path}.`),
          );
        return el(
          'transition',
          {
            event: event === null ? undefined : event,
            cond: t.guard === undefined ? undefined : this.expr(this.guardExpr(chart, t.guard)),
            target: targets.length ? targets.join(' ') : undefined,
            type: inside && !t.reenter ? 'internal' : undefined,
          },
          this.actions(chart, t.actions),
        );
      });
    }

    /** <state>/<parallel>/<final>/<history> for a node; `kids` overrides the children. */
    state(chart, node, extra = [], kids = node.children, machineOutput) {
      const c = node.config;
      const tag = { parallel: 'parallel', final: 'final', history: 'history' }[node.type] || 'state';
      if (tag === 'history') {
        const t = c.target ? this.resolveTarget(chart, node, c.target) : node.parent.children.find((k) => k !== node).path;
        return el('history', { id: node.id, type: c.history === 'deep' ? 'deep' : 'shallow' }, el('transition', { target: t }));
      }
      const attrs = { id: node.id };
      if (tag === 'state' && kids.length) attrs.initial = this.initialChild(node).id;
      // delayed transitions: send on entry, cancel on exit
      const afterEntries = [];
      const afterExits = [];
      const afterTransitions = [];
      for (const [delay, spec] of Object.entries(c.after || {})) {
        const ev = `xstate.after.${delay}.${node.id}`;
        const d = /^\d+(\.\d+)?$/.test(delay) ? this.delayAttrs(chart, Number(delay)) : this.delayAttrs(chart, delay);
        afterEntries.push(el('send', { event: ev, ...d, id: ev }));
        afterExits.push(el('cancel', { sendid: ev }));
        afterTransitions.push(this.transitions(chart, node, ev, spec));
      }
      const entry = [this.actions(chart, c.entry), afterEntries].flat(Infinity);
      const exit = [afterExits, this.actions(chart, c.exit)].flat(Infinity);
      const body = [];
      if (entry.length) body.push(el('onentry', null, entry));
      if (exit.length) body.push(el('onexit', null, exit));
      if (tag !== 'final') {
        for (const [event, spec] of Object.entries(c.on || {})) body.push(this.transitions(chart, node, event, spec));
        body.push(afterTransitions);
        if (c.always) body.push(this.transitions(chart, node, null, c.always));
        for (const inv of toArray(c.invoke)) body.push(this.invoke(chart, node, inv));
      }
      body.push(extra);
      if (tag === 'final') {
        const own = c.output !== undefined ? valueOf(c.output) : null;
        if (machineOutput !== undefined) {
          // the machine's output sees the final state's output as event.output
          const out = valueOf(machineOutput);
          const fn = own
            ? (args, $) => out.fn({ ...args, event: { type: `xstate.done.state.${node.id}`, output: own.fn(args, $) } }, $)
            : out.fn;
          body.push(this.donedata(X(out.text, fn, own ? `${out.key}<-${own.key}` : out.key)));
        } else if (own) {
          body.push(this.donedata(own));
        }
      }
      for (const k of kids) body.push(this.state(chart, k));
      return el(tag, attrs, body);
    }

    invoke(chart, node, inv) {
      const id = inv.id || `${node.id || 'machine'}.invoke${this.invokeSeq++}`;
      let src = inv.src;
      if (typeof src === 'string') {
        const named = chart.impl.actors && chart.impl.actors[src];
        if (!named) throw new Error(`unknown actor "${src}"`);
        src = named;
      }
      if (!src || !src.config) throw new Error('invoke.src must be a machine created by createMachine()');
      const child = this.machine(src);
      return [
        el(
          'invoke',
          { id },
          inv.input !== undefined && el('param', { name: 'input', expr: this.expr(valueOf(inv.input)) }),
          el('content', null, child),
        ),
        inv.onDone ? this.transitions(chart, node, `done.invoke.${id}`, inv.onDone) : null,
      ];
    }
  }

  /* ----------------------------------------------------------------- machines */

  class StateMachine {
    constructor(config, implementations = {}) {
      this.config = config;
      this.implementations = {
        actions: { ...(implementations.actions || {}) },
        guards: { ...(implementations.guards || {}) },
        delays: { ...(implementations.delays || {}) },
        actors: { ...(implementations.actors || {}) },
      };
      this.id = config.id || 'machine';
      this._compiled = null;
      registry.add(this);
    }
    /** A copy with additional/overridden implementations (like xstate's machine.provide). */
    provide(implementations) {
      const merged = {};
      for (const k of ['actions', 'guards', 'delays', 'actors'])
        merged[k] = { ...this.implementations[k], ...((implementations || {})[k] || {}) };
      return new StateMachine(this.config, merged);
    }
    /** The generated SCXML document. */
    toSCXML() {
      return this._compile().xml;
    }
    _compile() {
      if (!this._compiled) {
        const c = new Compiler();
        const xml = serialize(c.machine(this));
        this._compiled = { xml, registry: c.registry, chart: c.rootChart };
      }
      return this._compiled;
    }
  }

  /** Machines created so far; the default system links them all at once. */
  const registry = new Set();

  function createMachine(config, implementations) {
    return new StateMachine(config, implementations);
  }

  function setup(implementations = {}) {
    return {
      createMachine: (config) => createMachine(config, implementations),
    };
  }

  /* ------------------------------------------------------- SCXML documents
   * fromSCXML(xml, { id, documents }) runs a W3C SCXML document as actor
   * logic. Its datamodel attribute picks the semantics: "ecmascript"
   * (SCXML B.2, bindings/shared/ecmascript.mjs) or "null". `documents` maps
   * invoke src names to further SCXML documents linked alongside. Events sent
   * to such an actor: _event.name is `type`, _event.data the event's `data`
   * property, else the event's other properties (if any). The snapshot's
   * context is the datamodel's variables; its value the active states (by id)
   * as a nested state value. */
  const STATE_KINDS = ['atomic', 'compound', 'parallel', 'final', 'history', 'history'];

  class SCXMLLogic {
    constructor(xml, options = {}) {
      if (typeof xml !== 'string') throw new TypeError('fromSCXML(xml): xml must be a string');
      this.xml = xml;
      this.documents = options.documents || {};
      const m = /<scxml\b[^>]*?\sname\s*=\s*(["'])(.*?)\1/.exec(xml);
      this.id = options.id || (m && m[2]) || 'scxml';
      this.scxml = true;
      this._compiled = null;
    }
    toSCXML() {
      return this.xml;
    }
    /* compiled once on its own: validates the document and yields the state tree */
    _compile() {
      if (this._compiled) return this._compiled;
      /* SCXML documents become charts of the image; every document (also e.g.
       * <data src> text) is available to src="..." at compile time */
      const files = Object.entries(this.documents).map(([name, xml]) => ({ name, xml }));
      const docs = files.filter((f) => /<scxml[\s>]/.test(f.xml));
      const image = native.compile([{ name: '__main', xml: this.xml }, ...docs], files);
      const main = native.chartIndex(image, '__main');
      const dm = native.imageInfo(image).charts[main].datamodel;
      if (dm && dm !== 'ecmascript' && dm !== 'null') throw new Error(`fromSCXML: the "${dm}" datamodel is not supported (use "ecmascript" or "null")`);
      const states = native.states(image, main);
      const nodes = states.map((s) => ({ id: s.id, key: s.id, type: STATE_KINDS[s.kind] || 'atomic', children: [], tags: [] }));
      states.forEach((s, i) => s.parent >= 0 && nodes[s.parent].children.push(nodes[i]));
      const byPath = new Map();
      (function walk(n, prefix) {
        for (const k of n.children) {
          const p = prefix ? `${prefix}.${k.key}` : k.key;
          byPath.set(p, k);
          walk(k, p);
        }
      })(nodes[0], '');
      this._compiled = { xml: this.xml, registry: new Map(), chart: { root: nodes[0], nodes: byPath }, sources: docs, files };
      return this._compiled;
    }
    _eventData(event) {
      if (event.data !== undefined) return event.data;
      const { type, ...rest } = event;
      return Object.keys(rest).length ? rest : undefined;
    }
  }

  function fromSCXML(xml, options) {
    return new SCXMLLogic(xml, options);
  }

  /* --------------------------------------------------------- system images
   * A compiled system image (a <system> manifest: charts and the actors to
   * boot), from compileSystem() or loaded as bytes (loadImage(), e.g. what
   * the `tac` compiler wrote). system.boot(image) starts its actors;
   * image.chart(name) is logic for createActor. */
  const toNodes = (states) => {
    const nodes = states.map((s) => ({ id: s.id, key: s.id, type: STATE_KINDS[s.kind] || 'atomic', children: [], tags: [] }));
    states.forEach((s, i) => s.parent >= 0 && nodes[s.parent].children.push(nodes[i]));
    const byPath = new Map();
    (function walk(n, prefix) {
      for (const k of n.children) {
        const p = prefix ? `${prefix}.${k.key}` : k.key;
        byPath.set(p, k);
        walk(k, p);
      }
    })(nodes[0], '');
    return { root: nodes[0], nodes: byPath };
  };

  class ImageChart {
    constructor(image, chart, name) {
      this.image = image;
      this.id = name;
      this.scxml = true;
      this._index = chart;
      this._compiled = null;
    }
    _compile() {
      if (!this._compiled) this._compiled = { registry: new Map(), chart: toNodes(native.states(this.image._native, this._index)) };
      return this._compiled;
    }
    _eventData(event) {
      return SCXMLLogic.prototype._eventData.call(this, event);
    }
  }

  class SystemImage {
    constructor(nat) {
      this._native = nat;
      const info = native.imageInfo(nat);
      /** The charts: { name, src, datamodel }. */
      this.charts = info.charts;
      /** The actors system.boot() starts: { name, chart } (chart: an index into charts). */
      this.actors = info.bootActors;
      this._charts = info.charts.map((c, i) => new ImageChart(this, i, c.name || c.src));
    }
    /** The image as bytes (store it; loadImage() reads it back). */
    get bytes() {
      return native.imageBytes(this._native);
    }
    /** Actor logic for one of its charts (by <chart name>, or its src). */
    chart(name) {
      const i = this.charts.findIndex((c) => c.name === name || c.src === name);
      if (i < 0) throw new Error(`no chart "${name}" in the image`);
      return this._charts[i];
    }
  }

  /** Compile a <system> manifest; `files` maps the src names it uses to their text. */
  function compileSystem(manifest, options = {}) {
    const files = Object.entries(options.files || {}).map(([name, xml]) => ({ name, xml }));
    return new SystemImage(native.compileSystem(manifest, files));
  }

  /** A compiled image from bytes (compileSystem(...).bytes, or the tac compiler's output). */
  function loadImage(bytes) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError('loadImage(bytes: Uint8Array)');
    return new SystemImage(native.openImage(bytes));
  }

  /* ------------------------------------------------------------ actor systems */

  function addressOf(ref) {
    if (typeof ref === 'string') return `#_${ref}`; // child invoke id
    if (ref instanceof Actor) {
      if (!ref._live) throw new Error(`actor "${ref.id}" is not running`);
      return `${ref.system._base}#_scxml_${ref._session}`;
    }
    if (ref instanceof ActorRef) return ref.address;
    throw new Error('cannot address this actor');
  }

  /** A reference to an actor anywhere — this system, another segment, or a
   * system on the other side of a link. Sendable from host code, usable as a
   * sendTo() target, and it survives being sent in events (as its address). */
  class ActorRef {
    constructor(system, address) {
      this.system = system;
      this.address = address;
      const m = /@([^/]+)$/.exec(address);
      this.id = m ? m[1] : address;
    }
    /** `options.traceparent`: the step it starts continues that trace. */
    send(event, options = {}) {
      if (typeof event === 'string') event = { type: event };
      this.system._do(() => {
        this.system._hostRoute({ target: this.address, name: event.type, data: event, traceparent: options.traceparent });
        this.system._pump();
      });
    }
    toJSON() {
      return { $actor: this.address };
    }
  }

  /* ---- values across hibernation and links: tagged JSON. Keeps undefined,
   * NaN/Infinity, BigInt, Date, Map, Set, RegExp, shared references and cycles;
   * actors travel as their address. Functions cannot be encoded. */
  /* bytes as base64 (portable: no Buffer/btoa needed) */
  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  function toBase64(b) {
    let out = '';
    for (let i = 0; i < b.length; i += 3) {
      const n = (b[i] << 16) | ((b[i + 1] || 0) << 8) | (b[i + 2] || 0);
      out += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < b.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < b.length ? B64[n & 63] : '=');
    }
    return out;
  }
  function fromBase64(s) {
    const pad = s.endsWith('==') ? 2 : s.endsWith('=') ? 1 : 0;
    const out = new Uint8Array((s.length / 4) * 3 - pad);
    for (let i = 0, o = 0; i < s.length; i += 4) {
      const n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) | ((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
      out[o++] = n >> 16;
      if (o < out.length) out[o++] = (n >> 8) & 255;
      if (o < out.length) out[o++] = n & 255;
    }
    return out;
  }

  function encodeTagged(root) {
    const count = new Map();
    (function scan(v) {
      if (v === null || typeof v !== 'object' || v instanceof Actor || v instanceof ActorRef || v instanceof Uint8Array) return;
      const n = (count.get(v) || 0) + 1;
      count.set(v, n);
      if (n > 1) return;
      if (v instanceof Map) for (const [k, x] of v) scan(k), scan(x);
      else if (v instanceof Set) for (const x of v) scan(x);
      else if (!(v instanceof Date) && !(v instanceof RegExp)) for (const k of Object.keys(v)) scan(v[k]);
    })(root);
    const ids = new Map();
    let next = 0;
    function enc(v) {
      switch (typeof v) {
        case 'undefined': return { $: 'u' };
        case 'number': return Number.isFinite(v) ? v : { $: 'n', v: String(v) };
        case 'bigint': return { $: 'b', v: String(v) };
        case 'string': case 'boolean': return v;
        case 'function': case 'symbol': throw new TypeError(`cannot encode a ${typeof v}`);
      }
      if (v === null) return null;
      if (v instanceof Actor) return { $: 'a', v: addressOf(v) };
      if (v instanceof Uint8Array) return { $: 'y', v: toBase64(v) };
      if (v instanceof ActorRef) return { $: 'a', v: v.address };
      if (ids.has(v)) return { $: 'r', v: ids.get(v) };
      const shared = count.get(v) > 1;
      const id = shared ? next++ : -1;
      if (shared) ids.set(v, id);
      let out;
      if (v instanceof Date) out = { $: 'd', v: v.getTime() };
      else if (v instanceof RegExp) out = { $: 'x', v: v.source, f: v.flags };
      else if (v instanceof Map) out = { $: 'm', v: [...v].map(([k, x]) => [enc(k), enc(x)]) };
      else if (v instanceof Set) out = { $: 's', v: [...v].map(enc) };
      else if (Array.isArray(v)) out = shared ? { $: 'A', v: v.map(enc) } : v.map(enc);
      else {
        const o = {};
        for (const k of Object.keys(v)) o[k] = enc(v[k]);
        out = shared || '$' in v ? { $: 'o', v: o } : o;
      }
      if (shared) out.i = id;
      return out;
    }
    return enc(root);
  }
  function decodeTagged(root, system) {
    const ids = new Map();
    function dec(v) {
      if (v === null || typeof v !== 'object') return v;
      if (Array.isArray(v)) return v.map(dec);
      if (typeof v.$ !== 'string') {
        const o = {};
        for (const k of Object.keys(v)) o[k] = dec(v[k]);
        return o;
      }
      let out;
      switch (v.$) {
        case 'u': return undefined;
        case 'n': return Number(v.v);
        case 'b': return BigInt(v.v);
        case 'a': return system ? system.ref(v.v) : v.v;
        case 'r': return ids.get(v.v);
        case 'y': return fromBase64(v.v);
        case 'd': out = new Date(v.v); break;
        case 'x': out = new RegExp(v.v, v.f); break;
        case 'm': out = new Map(); if (v.i !== undefined) ids.set(v.i, out); for (const [k, x] of v.v) out.set(dec(k), dec(x)); return out;
        case 's': out = new Set(); if (v.i !== undefined) ids.set(v.i, out); for (const x of v.v) out.add(dec(x)); return out;
        case 'A': out = []; if (v.i !== undefined) ids.set(v.i, out); for (const x of v.v) out.push(dec(x)); return out;
        case 'o': out = {}; if (v.i !== undefined) ids.set(v.i, out); for (const k of Object.keys(v.v)) out[k] = dec(v.v[k]); return out;
        default: throw new TypeError(`unknown tag ${v.$}`);
      }
      if (v.i !== undefined) ids.set(v.i, out);
      return out;
    }
    return dec(root);
  }
  /* ---- neutral values (docs/protocol.md, "Values"): what other systems
   * understand whatever their language — JSON plus {"$ref": address},
   * {"$bytes": base64} and {"$": {...}} for objects with "$" keys. Lossy for
   * JS (no undefined, Map, Set, Date, cycles), so links between two JS systems
   * negotiate the richer "js" profile (the tagged JSON above). */
  function encodeWire(root) {
    const seen = new Set();
    function enc(v) {
      switch (typeof v) {
        case 'undefined': case 'function': case 'symbol': return undefined;
        case 'number': return Number.isFinite(v) ? v : null;
        case 'bigint': return String(v);
        case 'string': case 'boolean': return v;
      }
      if (v === null) return null;
      if (v instanceof Actor) return { $ref: addressOf(v) };
      if (v instanceof ActorRef) return { $ref: v.address };
      if (ArrayBuffer.isView(v)) return { $bytes: toBase64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
      if (v instanceof ArrayBuffer) return { $bytes: toBase64(new Uint8Array(v)) };
      if (typeof v.toJSON === 'function') return enc(v.toJSON());
      if (seen.has(v)) throw new TypeError('a cyclic value is not a neutral value');
      seen.add(v);
      let out;
      if (Array.isArray(v) || v instanceof Set) {
        out = [...v].map((x) => {
          const e = enc(x);
          return e === undefined ? null : e;
        });
      } else {
        const o = {};
        let dollar = false;
        for (const [k, x] of v instanceof Map ? [...v].map(([k, x]) => [String(k), x]) : Object.entries(v)) {
          const e = enc(x);
          if (e === undefined) continue;
          o[k] = e;
          if (k[0] === '$') dollar = true;
        }
        out = dollar ? { $: o } : o;
      }
      seen.delete(v);
      return out;
    }
    const e = enc(root);
    return e === undefined ? null : e;
  }
  function decodeWire(v, system) {
    if (v === null || typeof v !== 'object') return v;
    if (Array.isArray(v)) return v.map((x) => decodeWire(x, system));
    const keys = Object.keys(v);
    if (keys.length === 1) {
      const x = v[keys[0]];
      if (keys[0] === '$ref' && typeof x === 'string') return system ? system.ref(x) : x;
      if (keys[0] === '$bytes' && typeof x === 'string') return fromBase64(x);
      if (keys[0] === '$' && x && typeof x === 'object' && !Array.isArray(x)) {
        const o = {};
        for (const k of Object.keys(x)) o[k] = decodeWire(x[k], system);
        return o;
      }
    }
    const o = {};
    for (const k of keys) o[k] = decodeWire(v[k], system);
    return o;
  }

  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder();
  function encodeValue(v) {
    return textEncoder.encode(JSON.stringify(encodeTagged(v)));
  }
  function decodeValue(bytes, system) {
    return decodeTagged(JSON.parse(textDecoder.decode(bytes)), system);
  }
  const sameBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

  /** A hibernation store kept in memory (tests, or as a write-behind cache). */
  function createMemoryStore() {
    const m = new Map();
    return {
      get: (key) => m.get(key),
      set: (key, value) => void m.set(key, value),
      delete: (key) => void m.delete(key),
      get size() {
        return m.size;
      },
    };
  }

  /* ---------------------------------------------------------------- addresses
   * docs/protocol.md, "Addresses" — the JS twin of include/tinyactors/address.h, held
   * to the same vectors (tests/conformance/addresses.json). Session ids are
   * BigInt: exact up to 2^64-1. */
  const SYSTEM_ID = /^(?=.{1,253}$)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*$/;
  const MAX_SESSION = (1n << 64n) - 1n;
  function systemIDValid(id) {
    return typeof id === 'string' && SYSTEM_ID.test(id);
  }
  function parseLocal(local) {
    let m = /^#_scxml_([1-9][0-9]{0,19})$/.exec(local);
    if (m) {
      const session = BigInt(m[1]);
      return session <= MAX_SESSION ? { kind: 'session', session } : null;
    }
    m = /^@((?:[A-Za-z0-9\-._~]|%[0-9A-Fa-f]{2})+)$/.exec(local);
    if (!m) return null;
    try {
      return { kind: 'name', name: decodeURIComponent(m[1]), encoded: m[1] };
    } catch {
      return null; /* escapes that are not UTF-8 */
    }
  }
  function parseAddress(text) {
    const m = /^tinyactors:\/\/([^/]*)\/(.*)$/s.exec(typeof text === 'string' ? text : '');
    if (!m || !systemIDValid(m[1])) return null;
    const local = parseLocal(m[2]);
    return local ? { system: m[1], local: m[2], ...local } : null;
  }
  /* IDs from the wire are BigInt (exact up to 2^64-1). This host numbers its
   * own sessions segment << 32 | counter, below 2^53, so they fit a JS number
   * in calls to the native host; a larger id names no session here (0). */
  function hostSession(id) {
    return id <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(id) : 0;
  }
  function formatAddress(system, local) {
    if (!systemIDValid(system)) throw new TypeError(`not a system id: ${system}`);
    if (typeof local === 'bigint' || typeof local === 'number') {
      const s = BigInt(local);
      if (s < 1n || s > MAX_SESSION) throw new RangeError('session ids are 1 .. 2^64-1');
      return `tinyactors://${system}/#_scxml_${s}`;
    }
    const enc = new TextEncoder().encode(String(local));
    if (!enc.length) throw new TypeError('empty name');
    let out = '';
    for (const b of enc) {
      const c = String.fromCharCode(b);
      out += /[A-Za-z0-9\-._~]/.test(c) ? c : `%${b.toString(16).toUpperCase().padStart(2, '0')}`;
    }
    return `tinyactors://${system}/@${out}`;
  }

  const randomID = () =>
    (globalThis.crypto && globalThis.crypto.randomUUID ? globalThis.crypto.randomUUID() : `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`)
      .replace(/-/g, '')
      .slice(0, 12);

  /* ---------------------------------------------------------------- tracing
   * Core writes trace records (docs/adr/0006 in the repository root); the
   * binding's C side encodes them as OTLP protobuf after every run, and this
   * code hands the bytes to a sink and to an OTLP/HTTP exporter, as they
   * are: no object per record or span. */

  /** Core's record kinds, in core's order (TA_TRACE_* in tinyactors/trace.h). */
  const TRACE_KINDS = Object.freeze([
    'dropped', 'spawned', 'finished', 'destroyed', 'unloaded', 'loaded', 'macrostepBegin', 'macrostepEnd',
    'microstep', 'condition', 'raised', 'sent', 'invoked', 'cancelled', 'assign', 'log', 'actionFailed',
  ]);
  const kindsMask = (kinds) =>
    kinds.reduce((mask, kind) => {
      const i = TRACE_KINDS.indexOf(kind);
      if (i < 0) throw new TypeError(`not a trace kind: ${kind} (one of ${TRACE_KINDS.join(', ')})`);
      return (mask | (1 << (i + 1))) >>> 0;
    }, 0);
  const LIFECYCLE = ['spawned', 'finished', 'destroyed', 'unloaded', 'loaded'];
  const SUMMARY = [...LIFECYCLE, 'macrostepBegin', 'macrostepEnd', 'actionFailed'];
  const DECISIONS = [...SUMMARY, 'microstep', 'condition', 'raised'];
  const DETAIL = {
    summary: kindsMask(SUMMARY),
    decisions: kindsMask(DECISIONS),
    full: kindsMask([...DECISIONS, 'sent', 'invoked', 'cancelled', 'assign', 'log']),
  };
  const TRACE_VALUES = 2 ** 31; /* TA_TRACE_VALUES: event data and assigned values */
  const OTLP_LINK_PARENTS = 1, OTLP_VALUES = 2, OTLP_NO_LOGS = 4; /* TA_OTLP_* */
  const SAMPLE_WHOLE = 1000000;
  const EXPORT_DELAY_MS = 250; /* bytes gather this long before a post */
  const EXPORT_BYTES = 1 << 20; /* or until this many wait */
  const EXPORT_MAX_BYTES = 16 << 20; /* waiting beyond this is dropped */
  const FAILURE_LOG_MS = 10000;

  function traceKinds(options) {
    let kinds;
    if (options.kinds) kinds = kindsMask(options.kinds);
    else {
      kinds = DETAIL[options.detail || 'full'];
      if (kinds === undefined) throw new TypeError(`detail: "summary", "decisions" or "full", not ${options.detail}`);
    }
    return options.values ? (kinds | TRACE_VALUES) >>> 0 : kinds;
  }
  function sampleParts(sample) {
    if (sample === undefined) return SAMPLE_WHOLE;
    if (typeof sample !== 'number' || !(sample >= 0 && sample <= 1)) throw new RangeError('sample: a ratio from 0 to 1');
    return Math.round(sample * SAMPLE_WHOLE);
  }
  /* the trace ID generator's seed, as two 32-bit halves */
  function traceSeed(seed) {
    if (seed === undefined) {
      const words = new Uint32Array(2);
      if (globalThis.crypto && globalThis.crypto.getRandomValues) globalThis.crypto.getRandomValues(words);
      else for (let i = 0; i < 2; i++) words[i] = Math.floor(Math.random() * 2 ** 32);
      return [words[0], words[1]];
    }
    const value = BigInt.asUintN(64, BigInt(seed));
    return [Number(value >> 32n), Number(value & 0xffffffffn)];
  }
  const wallClock = () => (globalThis.performance && performance.timeOrigin ? performance.timeOrigin + performance.now() : Date.now());

  /* concatenated protobuf messages of one type are one message (their repeated fields append) */
  function concatBytes(parts) {
    if (parts.length === 1) return parts[0];
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    n = 0;
    for (const p of parts) {
      out.set(p, n);
      n += p.length;
    }
    return out;
  }

  const fetchPost = (url, headers, body) =>
    globalThis.fetch(url, { method: 'POST', headers: { 'content-type': 'application/x-protobuf', ...headers }, body }).then((response) => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
    });

  /* Posts OTLP protobuf to /v1/traces and /v1/logs: gathered for a moment,
   * one request in flight per signal; failures are quiet, logged at most
   * once every ten seconds. */
  class Exporter {
    constructor(options) {
      const base = options.endpoint ? String(options.endpoint).replace(/\/+$/, '') : '';
      this.urls = {
        traces: options.tracesEndpoint || (base && `${base}/v1/traces`),
        logs: options.logsEndpoint || (base && `${base}/v1/logs`),
      };
      this.headers = { ...(options.headers || {}) };
      this.post = options.post || platform.post || fetchPost;
      this.waiting = { traces: [], logs: [] };
      this.bytes = { traces: 0, logs: 0 };
      this.busy = { traces: false, logs: false };
      this.timer = null;
      this.lastFailure = -Infinity;
      this.posted = 0;
      this.failed = 0;
      this.idle = [];
    }
    add(signal, bytes) {
      if (!bytes || !this.urls[signal]) return;
      if (this.bytes[signal] + bytes.length > EXPORT_MAX_BYTES) return this.fail(new Error(`more than ${EXPORT_MAX_BYTES} bytes waiting`));
      this.waiting[signal].push(bytes);
      this.bytes[signal] += bytes.length;
      if (this.bytes[signal] >= EXPORT_BYTES) this.flush();
      else if (!this.timer) this.timer = platform.setTimer(() => this.flush(), EXPORT_DELAY_MS);
    }
    flush() {
      platform.clearTimer(this.timer);
      this.timer = null;
      for (const signal of ['traces', 'logs']) {
        if (this.busy[signal] || !this.waiting[signal].length) continue;
        const body = concatBytes(this.waiting[signal].splice(0));
        this.bytes[signal] = 0;
        this.busy[signal] = true;
        let sent;
        try {
          sent = Promise.resolve(this.post(this.urls[signal], this.headers, body));
        } catch (e) {
          sent = Promise.reject(e);
        }
        sent.then(
          () => this.posted++,
          (e) => this.fail(e),
        ).then(() => {
          this.busy[signal] = false;
          if (this.waiting[signal].length) this.flush();
          else if (!this.busy.traces && !this.busy.logs && !this.waiting.traces.length && !this.waiting.logs.length)
            for (const resolve of this.idle.splice(0)) resolve();
        });
      }
    }
    fail(e) {
      this.failed++;
      const now = platform.now();
      if (now - this.lastFailure < FAILURE_LOG_MS) return;
      this.lastFailure = now;
      platform.log(`tinyactors: exporting traces failed (${this.failed} so far): ${e && e.message ? e.message : e}`);
    }
    /* resolves once nothing waits and nothing is in flight */
    settled() {
      this.flush();
      if (!this.busy.traces && !this.busy.logs) return Promise.resolve();
      return new Promise((resolve) => this.idle.push(resolve));
    }
  }

  const SPAN_KINDS = ['unspecified', 'internal', 'server', 'client', 'producer', 'consumer'];
  const STATUS_CODES = ['unset', 'ok', 'error'];
  function decodedValue(v) {
    if (v === null || typeof v !== 'object') return v;
    if (Array.isArray(v)) return v.map(decodedValue);
    if ('int' in v) return BigInt(v.int);
    if ('list' in v) return decodedAttributes(v.list);
    if ('bytes' in v) return v.bytes;
    return v;
  }
  function decodedAttributes(pairs) {
    const out = {};
    for (const [key, value] of pairs) out[key] = decodedValue(value);
    return out;
  }
  const millis = (ns) => Number(ns / 1000n) / 1000;

  /** Reads OTLP protobuf trace bytes (C's decoder); throws on a malformed message. */
  function decodeTraces(bytes) {
    return native.decodeTraces(bytes).map((s) => {
      const start = BigInt(s.start), end = BigInt(s.end);
      const span = {
        traceID: s.traceID,
        spanID: s.spanID,
        name: s.name,
        kind: SPAN_KINDS[s.kind] || 'unspecified',
        startTime: millis(start),
        endTime: millis(end),
        startTimeUnixNano: start,
        endTimeUnixNano: end,
        attributes: decodedAttributes(s.attributes),
        events: s.events.map((e) => {
          const time = BigInt(e.time);
          return { name: e.name, time: millis(time), timeUnixNano: time, attributes: decodedAttributes(e.attributes) };
        }),
        links: s.links.map((l) => ({ traceID: l.traceID, spanID: l.spanID, attributes: decodedAttributes(l.attributes) })),
        status: s.statusMessage ? { code: STATUS_CODES[s.status] || 'unset', message: s.statusMessage } : { code: STATUS_CODES[s.status] || 'unset' },
        resource: { attributes: decodedAttributes(s.resource) },
        scope: { name: s.scopeName, version: s.scopeVersion, attributes: decodedAttributes(s.scopeAttributes) },
      };
      if (s.parentSpanID) span.parentSpanID = s.parentSpanID;
      return span;
    });
  }

  /** A W3C traceparent's fields (C's parser), or undefined when it is not a valid one. */
  function parseTraceparent(header) {
    const p = native.parseTraceparent(header);
    return p ? { traceID: p.traceID, parentSpanID: p.parentSpanID, flags: p.flags, sampled: (p.flags & 1) === 1 } : undefined;
  }

  /** A traceparent naming `span` as the parent (sampled unless `sampled: false` or `flags` says). */
  function formatTraceparent({ traceID, spanID, sampled, flags } = {}) {
    const text = native.formatTraceparent(String(traceID), String(spanID), flags !== undefined ? flags : sampled === false ? 0 : 1);
    if (text === null) throw new TypeError('formatTraceparent: a 32-digit traceID and a 16-digit spanID, in hex, neither all zeros');
    return text;
  }

  const decimal = (v, what) => {
    const m = /^\D*(\d+)$/.exec(String(v));
    if (!m) throw new TypeError(`stepSpanID: ${what} is not a number`);
    return m[1];
  };
  /** The span ID of a step (C's mix): session, macrostep, microstep (0: the macrostep's span). */
  function stepSpanID(session, macrostep, microstep = 0) {
    return native.stepSpanID(decimal(session, 'session'), decimal(macrostep, 'macrostep'), microstep >>> 0);
  }

  class System {
    constructor(options = {}) {
      this.options = options;
      /** This system's name in actor addresses (tinyactors://<id>/...); unique across links. */
      const restored = options.restore ? decodeValue(options.restore, null) : null;
      if (restored && restored.format !== 'tinyactors-system-snapshot') throw new TypeError('options.restore is not a system snapshot');
      this.id = (restored && restored.id) || options.id || randomID();
      if (!systemIDValid(this.id)) throw new TypeError(`createSystem: "${this.id}" is not a valid system id (lowercase a-z, 0-9, "-", dots; docs/protocol.md)`);
      this._base = `tinyactors://${this.id}/`;
      this._peers = new Map(); /* router peer handle -> the transport connection moving its envelopes */
      this._links = new Set(); /* client links (closed with the system) */
      this._refs = new Map(); /* address -> ActorRef */
      const hib = options.hibernation;
      this._store = hib ? hib.store || createMemoryStore() : null;
      this._pending = new Map(); /* session -> blob while the store is writing it */
      this._sleeping = new Map(); /* session -> root Actor (for cached snapshots) */
      this._waking = new Set(); /* sessions whose blob is being loaded */
      this._stats = { hibernations: 0, wakes: 0 };
      if (hib && hib.sweepMs !== 0) this._armSweep(hib.sweepMs || 30000);
      this._images = []; /* by segment: the compiled image */
      this._sources = new Map(); /* source name -> compiled machine */
      this._linked = new Map(); /* machine -> { segment, chart } */
      this._byChart = new Map(); /* `${segment}:${chart}` -> machine */
      this._versions = new Map(); /* machine id -> { version, machine } after reload() */
      this._actors = new Map(); /* session -> Actor (started with createActor) */
      this._ids = new Map(); /* systemID -> Actor */
      this._observed = new Set();
      this._jobs = new Map(); /* migration job -> { result, done } */
      this._settle = []; /* resolvers called once the system is idle */
      this._queue = [];
      this._running = false;
      this._dirty = false;
      this._yielded = false;
      this._deadline = -1;
      this._timer = null;
      this._immediate = null;
      this._clockTimers = null; /* { at, fn } on this system's clock (with a `clock` option) */
      this._version = 0;
      this._t0 = platform.now();
      this._budget = BUDGET;
      this._api = { get: (id) => this._ids.get(id) };
      this._segInfo = []; /* by segment: what it was made from (for snapshots) */
      this._imageSegs = new Map(); /* SystemImage -> segment */
      /* the native host: the datamodels and a ta_host — segments, routing between
       * them, versions, migration jobs and the hibernation sweep live in C */
      const callbacks = [(chart, texts) => this._resolve(chart, texts), { encode: (v) => encodeValue(v), decode: (b) => decodeValue(b, this) }, this.id, createEcmascript()];
      const traces = options.traces || {};
      const hostOptions = { clock: platform.wallClock || wallClock, seed: traceSeed(traces.seed) };
      if (restored) this._restoreFrom(restored, callbacks, hostOptions);
      else this._host = native.createHost(...callbacks, hostOptions);
      /* tracing: the platform's exporter settings (Node.js: OTEL_* variables), the application's over them */
      const telemetry = options.telemetry === false ? { disabled: true } : { ...(platform.telemetry || {}), ...(options.telemetry || {}) };
      this._telemetry = telemetry;
      this._exporter = !telemetry.disabled && (telemetry.endpoint || telemetry.tracesEndpoint) ? new Exporter(telemetry) : null;
      this._traceSink = null;
      this._traceOptions = {};
      this._traceApply();
      if (traces.sink) this.setTraceSink(traces.sink, traces);
    }

    /* ---- tracing ---- */

    /**
     * Hand traces to `sink` as OTLP protobuf: per run, one Uint8Array with
     * one ExportTraceServiceRequest (the spans that ended), and one with an
     * ExportLogsServiceRequest (failures and <log>) or undefined. `null`
     * removes it. Options: detail ("summary" | "decisions" | "full"), kinds,
     * values, valueLimit, sample, parent ("cause" | "link"), resource.
     */
    setTraceSink(sink, options = {}) {
      if (sink !== null && sink !== undefined && typeof sink !== 'function') throw new TypeError('setTraceSink(sink: (traces, logs) => void, options)');
      if (this._stopped) throw new Error('the system was stopped');
      this._traceSink = sink || null;
      this._traceOptions = { ...options };
      delete this._traceOptions.sink;
      delete this._traceOptions.seed; /* (the seed is given when the system is created: options.traces.seed) */
      this._traceApply();
    }

    /** Change detail, kinds and sample while traces flow; open spans are kept. */
    configureTraces({ detail, kinds, sample } = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const next = { ...this._traceOptions };
      if (detail !== undefined || kinds !== undefined) {
        next.detail = detail;
        next.kinds = kinds;
      }
      if (sample !== undefined) next.sample = sample;
      this._traceOptions = next;
      this._traceApply();
    }

    /** Resolves once every trace export this system started has finished. */
    flushTraces() {
      return this._exporter ? this._exporter.settled() : Promise.resolve();
    }

    _traceApply() {
      const t = this._traceOptions;
      const exporter = this._exporter;
      const sample = t.sample !== undefined ? t.sample : this._telemetry.sample;
      let flags = 0;
      if (t.parent === 'link') flags |= OTLP_LINK_PARENTS;
      else if (t.parent !== undefined && t.parent !== 'cause') throw new TypeError('parent: "cause" or "link"');
      if (t.values) flags |= OTLP_VALUES;
      if (t.logs === false) flags |= OTLP_NO_LOGS;
      const resource = { 'service.name': 'tinyactors', ...(this._telemetry.resource || {}), ...(t.resource || {}), 'tinyactors.system.id': this.id };
      /* with nowhere for records to go core writes none; contexts still flow (traceparents) */
      native.traceConfigure(this._host, {
        enabled: !!(this._traceSink || exporter),
        kinds: traceKinds(t),
        sampleParts: sampleParts(sample),
        sampleWhole: SAMPLE_WHOLE,
        flags,
        valueLimit: t.valueLimit || 0,
        resource: Object.entries(resource).map(([k, v]) => [String(k), String(v)]),
      });
    }

    _traces(traces, logs) {
      if (!traces && !logs) return;
      if (this._exporter) {
        this._exporter.add('traces', traces);
        this._exporter.add('logs', logs);
      }
      if (this._traceSink) this._traceSink(traces || new Uint8Array(0), logs);
    }

    /* ---- system images ---- */
    _linkImage(image) {
      let segment = this._imageSegs.get(image);
      if (segment !== undefined) return segment;
      native.advance(this._host, this._now());
      segment = native.addImage(this._host, image._native);
      this._images[segment] = image._native;
      this._imageSegs.set(image, segment);
      this._segInfo[segment] = { kind: 'image', bytes: image.bytes };
      image._charts.forEach((logic, chart) => {
        this._linked.set(logic, { segment, chart });
        this._byChart.set(`${segment}:${chart}`, logic);
      });
      return segment;
    }

    /** Start the actors a system image declares (<system><actor name chart/>);
     * returns them by name. */
    boot(image) {
      if (!(image instanceof SystemImage)) throw new TypeError('boot() expects an image from compileSystem() or loadImage()');
      if (this._stopped) throw new Error('the system was stopped');
      const out = {};
      this._do(() => {
        const segment = this._linkImage(image);
        if (!native.boot(this._host, segment)) throw new Error('cannot boot the image');
        for (const { name, chart } of image.actors) {
          const session = native.actorByName(this._host, name);
          if (!session) continue;
          const actor = new Actor(image._charts[chart], { system: this, id: name, systemID: name });
          this._adopt(actor, session);
          out[name] = actor;
        }
        this._dirty = true;
        this._pump();
      });
      return out;
    }

    /* an Actor object for a session the native host already runs */
    _adopt(actor, session, hibernated = false) {
      actor._started = true;
      actor._live = true;
      actor._session = actor._serial = session;
      actor._chart = actor.logic._compile().chart;
      actor._hibernated = hibernated;
      this._actors.set(session, actor);
      if (hibernated) this._sleeping.set(session, actor);
      if (actor.options.systemID) this._ids.set(actor.options.systemID, actor);
    }

    /* ---- snapshot / restore ---- */

    /**
     * The whole system as bytes: every actor (state, variables, queued events,
     * timers), and what it runs (machine XML, SCXML documents, images).
     * Restore with createSystem({ restore, machines }). Needs a quiet moment:
     * not during a migration; context values must be encodable (no functions).
     * Hibernated actors stay in the hibernation store: restore with the same store.
     */
    snapshot() {
      if (this._stopped) throw new Error('the system was stopped');
      if (this._running) throw new Error('snapshot() cannot run inside an action');
      if (this._jobs.size) throw new Error('snapshot() cannot run during a migration');
      const { host, dm } = native.snapshot(this._host);
      const keyOf = (logic) => {
        const l = this._linked.get(logic);
        return l ? `${l.segment}:${l.chart}` : null;
      };
      return encodeValue({
        format: 'tinyactors-system-snapshot',
        version: 1,
        id: this.id,
        now: this._now(),
        segments: this._segInfo.map((info, segment) => (this._images[segment] ? info : null)),
        versions: [...this._versions].map(([id, v]) => ({ id, version: v.version })),
        actors: [...this._actors.values()].filter((a) => a._live).map((a) => ({ session: a._session, id: a.id, systemID: a.options.systemID, logic: keyOf(a.logic), hibernated: a._hibernated })),
        host,
        dm: encodeValue(dm), /* decoded once the restoring system exists (actor references) */
      });
    }

    /* the native image of a segment as recorded in a snapshot or an export, and its
     * charts' logic: [chart, logic, compiled machine or null] */
    _segmentImage(info, machines = [], what = 'restore: machine "%" as of the snapshot', lenient = false) {
      if (info.kind === 'image') {
        const image = loadImage(info.bytes);
        const links = [];
        image._charts.forEach((logic, chart) => links.push([chart, logic, null, image]));
        return { image: image._native, links, missing: [] };
      }
      const missing = [];
      const all = [...machines, ...(this.options.machines || []), ...registry];
      const sources = info.sources.map((src) => {
        let logic = null;
        if (src.logic && src.logic.kind === 'scxml') logic = new SCXMLLogic(src.logic.xml, { id: src.logic.id, documents: src.logic.documents });
        else if (src.logic) {
          logic = all.find((m) => m instanceof StateMachine && m.id === src.logic.id && m._compile().xml === src.xml);
          if (!logic) {
            const why = `${what.replace('%', src.logic.id)} is not defined (create it, or pass it in options.machines; or it changed since: restore with the old version, then reload())`;
            if (!lenient) throw new Error(why);
            missing.push([src.name, why]);
          }
        }
        return { ...src, logic };
      });
      const image = native.compile(sources.map(({ name, xml }) => ({ name, xml })), info.files || []);
      const links = sources.filter((s) => s.logic).map((s) => [native.chartIndex(image, s.name), s.logic, s.logic._compile(), null, s.name]);
      return { image, links, missing: missing.map(([name, why]) => [native.chartIndex(image, name), why]) };
    }

    _register(segment, info, links) {
      this._segInfo[segment] = info;
      for (const [chart, logic, compiled, image, name] of links) {
        if (image) this._imageSegs.set(image, segment);
        if (compiled) this._sources.set(name, compiled);
        this._linked.set(logic, { segment, chart });
        this._byChart.set(`${segment}:${chart}`, logic);
      }
    }

    _restoreFrom(rec, callbacks, hostOptions) {
      const images = rec.segments.map((info, segment) => {
        if (!info) return null;
        const { image, links } = this._segmentImage(info);
        this._register(segment, info, links);
        return image;
      });
      this._host = native.restore(...callbacks, images, rec.host, decodeValue(rec.dm, this), hostOptions);
      images.forEach((im, segment) => (this._images[segment] = im || undefined));
      for (const v of rec.versions || []) {
        let machine = null;
        for (const [key, logic] of this._byChart) if (logic.id === v.id && (!machine || Number(key.split(':')[0]) > machine.seg)) machine = { logic, seg: Number(key.split(':')[0]) };
        if (machine) this._versions.set(v.id, { version: v.version, machine: machine.logic });
      }
      for (const a of rec.actors) {
        const logic = a.logic && this._byChart.get(a.logic);
        if (!logic) continue;
        this._adopt(new Actor(logic, { system: this, id: a.id, systemID: a.systemID }), a.session, a.hibernated);
      }
      this._t0 = platform.now() - rec.now; /* (with a clock option, the clock says what time it is) */
      native.advance(this._host, this._now());
      this._dirty = true;
      this._immediate = platform.defer(() => this._pump());
    }

    /** A reference to the actor at `address` (a local Actor when it is one of ours). */
    ref(address) {
      if (address instanceof Actor || address instanceof ActorRef) return address;
      const m = /^tinyactors:\/\/([^/]+)\/#_scxml_(\d+)$/.exec(address);
      if (m && m[1] === this.id) {
        const a = this._actors.get(Number(m[2]));
        if (a) return a;
      }
      let r = this._refs.get(address);
      if (!r) this._refs.set(address, (r = new ActorRef(this, address)));
      return r;
    }

    /* ---- other systems: routing decisions are the native router's (ta_host:
     * peers, routes, relaying, origin checks, policy, dedup — docs/protocol.md);
     * transports only move envelopes between it and the wire. */

    /** Hand an envelope (docs/protocol.md, "Envelope": target, name, data,
     * origin, sendid, msgid, dedup, hops) to the router, as this system's host
     * code: delivered to a local actor, or to the peer its target routes to.
     * Returns 'delivered', 'relayed', 'duplicate' or 'refused'. */
    deliver(envelope) {
      if (!envelope || typeof envelope.target !== 'string' || typeof envelope.name !== 'string') throw new TypeError('deliver({ target, name, ... })');
      let out = -1;
      this._do(() => {
        native.advance(this._host, this._now());
        out = native.deliverEnvelope(this._host, 0, { origin: formatAddress(this.id, 'host'), ...envelope });
        this._dirty = true;
        this._pump();
      });
      return ['delivered', 'relayed', 'duplicate', 'refused'][out] || (out === -1 && this._running ? 'queued' : 'refused');
    }

    /* an envelope from this system's host code (no origin checks): delivered
     * here or handed to the peer its target routes to */
    _hostRoute(env) {
      native.advance(this._host, this._now());
      const out = native.deliverEnvelope(this._host, 0, { origin: formatAddress(this.id, 'host'), ...env });
      this._dirty = true;
      return out === 0 || out === 1;
    }

    /* a frame from the transport of router peer `peer`: `ev` (an envelope) or `err` */
    _fromPeer(peer, msg) {
      this._do(() => {
        if (!this._peers.has(peer)) return;
        native.advance(this._host, this._now());
        if (msg.t === 'ev') {
          const conn = this._peers.get(peer);
          native.deliverEnvelope(this._host, peer, {
            target: str(msg.target),
            name: str(msg.name),
            data: msg.data === undefined ? undefined : conn.decode(msg.data),
            origin: str(msg.origin),
            origintype: str(msg.origintype),
            sendid: str(msg.sendid),
            msgid: str(msg.msgid),
            dedup: str(msg.dedup),
            hops: Number.isInteger(msg.hops) && msg.hops >= 0 ? msg.hops : 0,
            traceparent: str(msg.traceparent),
          });
        } else if (msg.t === 'err') {
          const reason = REASONS.indexOf(msg.reason);
          native.peerError(this._host, peer, str(msg.origin), str(msg.msgid), str(msg.sendid), reason > 0 ? reason : REASONS.indexOf('unreachable'));
        }
        this._dirty = true;
        this._pump();
      });
    }

    /* a transport connection carries router peer conn.peer (from now on) */
    _attach(conn) {
      this._peers.set(conn.peer, conn);
    }

    _detach(conn) {
      if (this._peers.get(conn.peer) === conn) this._peers.delete(conn.peer);
    }

    /* the peer said hello: what it reaches, and it is up (queued envelopes go out) */
    _peerHello(conn, reach) {
      this._do(() => {
        native.peerReach(this._host, conn.peer, reach);
        native.peerUp(this._host, conn.peer, true);
        this._dirty = true;
        this._pump();
        this._announce();
      });
    }

    _peerDown(conn, remove) {
      this._do(() => {
        if (remove) {
          native.advance(this._host, this._now());
          native.peerRemove(this._host, conn.peer); /* queued envelopes fail: error.communication */
          this._detach(conn);
        } else native.peerUp(this._host, conn.peer, false);
        this._dirty = true;
        this._pump();
        this._announce();
      });
    }

    /* the systems reachable through us, as announced to `conn` (links with
     * announce: true, or a list: only those): our other open peers and what
     * they reach */
    _reachFor(conn) {
      const out = new Set();
      for (const c of new Set(this._peers.values())) {
        if (c === conn || !c.open || !c.remoteID) continue;
        out.add(c.remoteID);
        for (const r of c.reach) out.add(r);
      }
      out.delete(this.id);
      if (conn.remoteID) out.delete(conn.remoteID);
      const only = Array.isArray(conn.options.announce) ? new Set(conn.options.announce) : null;
      return [...out].filter((id) => !only || only.has(id)).sort();
    }

    /* re-announce (a repeated hello) where what we reach changed */
    _announce() {
      for (const c of new Set(this._peers.values())) if (c.announce && c.open) c.hello();
    }

    /* ---- moving actors between systems (another process, Node or web) ---- */

    /**
     * Move `actor` out of this system, as bytes that another system — in this
     * or another process — takes in with adopt(): its states, variables,
     * delayed events with their data, the actors it invoked, and what it runs
     * (machine SCXML, documents or image). The actor stops here, without a done
     * event. It must be awake, not in a step, with nothing queued for it.
     */
    export(actor) {
      if (!(actor instanceof Actor) || actor.system !== this) throw new TypeError('export() expects an actor of this system');
      if (this._stopped) throw new Error('the system was stopped');
      if (this._running) throw new Error('export() cannot run inside an action');
      if (!actor._live || actor._status !== 'active') throw new Error('export(): the actor is not running');
      if (actor._hibernated) throw new Error('export(): the actor is hibernated (send it an event to wake it first)');
      native.advance(this._host, this._now());
      const snap = actor._makeSnapshot();
      const r = native.exportActor(this._host, actor._session);
      if (!r) {
        const why = native.storeError(this._host);
        throw new Error(`export(): ${why ? why.message || why : 'the actor is busy (events or requests queued for it): export it once the system is idle'}`);
      }
      const rec = {
        format: 'tinyactors-actor',
        version: 1,
        id: actor.id,
        systemID: actor.options.systemID,
        now: this._now(), /* the adopting system keeps what its timers had left */
        segment: this._segInfo[r.segment],
        chart: r.chart,
        blob: r.blob,
      };
      this._forget(actor);
      actor._snapshot = { ...snap, status: 'stopped' };
      actor._hibernated = true; /* (no session to read from any more) */
      actor._halt();
      actor._hibernated = false;
      return encodeValue(rec);
    }

    /**
     * Take in an actor another system exported: the same session (its address
     * under this system's id), states and variables; delayed events keep the
     * time they had left. Its machine is found as for createSystem({ restore })
     * (defined in this process or in `options.machines`); SCXML documents and
     * images travel with it.
     */
    adopt(bytes, options = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      if (this._running) throw new Error('adopt() cannot run inside an action');
      let rec = null;
      try {
        rec = decodeValue(bytes, null);
      } catch {
        /* not ours */
      }
      if (!rec || rec.format !== 'tinyactors-actor') throw new TypeError('adopt() expects bytes from system.export()');
      const want = encodeValue(rec.segment);
      let segment = this._segInfo.findIndex((info, i) => info && this._images[i] && sameBytes(encodeValue(info), want));
      if (segment < 0) {
        /* (only the actor's own machine must be defined here) */
        const { image, links, missing } = this._segmentImage(rec.segment, options.machines, 'adopt(): machine "%" as exported', true);
        const own = missing.find(([chart]) => chart === rec.chart);
        if (own) throw new Error(own[1]);
        for (const [, , compiled, , name] of links) if (compiled) this._sources.set(name, compiled); /* (resolved when linked) */
        native.advance(this._host, this._now());
        segment = native.addImage(this._host, image);
        this._images[segment] = image;
        this._register(segment, rec.segment, links);
      }
      const logic = this._byChart.get(`${segment}:${rec.chart}`);
      if (!logic) throw new Error('adopt(): the actor\'s machine is not known here');
      native.advance(this._host, this._now());
      const session = native.adopt(this._host, segment, rec.blob, rec.now, this._now());
      const actor = new Actor(logic, { system: this, id: rec.id, systemID: rec.systemID });
      this._adopt(actor, session);
      this._dirty = true;
      this._pump();
      return actor;
    }

    /* ---- hibernation (the system never does I/O: blobs go to options.hibernation.store) */
    _armSweep(ms) {
      const t = platform.setTimer(() => {
        this._do(() => {
          this.hibernate();
          this._pump();
        });
        this._armSweep(ms);
      }, ms);
      if (t && typeof t.unref === 'function') t.unref();
      this._sweepTimer = t;
    }

    /** Hibernate `actor` now, or (no argument) every actor that has been idle
     * since the previous sweep. Returns how many were unloaded. An actor whose
     * data cannot be stored throws (the message names the variable); the sweep
     * leaves such actors resident. */
    hibernate(actor) {
      if (!this._store) throw new Error('createSystem({ hibernation: { store } }) is needed to hibernate');
      if (this._running) return 0;
      let n = 0;
      const one = (session) => {
        const key = String(session);
        const root = this._actors.get(session);
        if (root) root._snapshot = root._makeSnapshot(); /* answered from cache while asleep */
        const blob = native.hibernate(this._host, session);
        if (!blob) {
          const why = native.storeError(this._host);
          if (why && actor) throw new Error(`cannot hibernate the actor: ${why.message || why}`);
          return;
        }
        n++;
        this._stats.hibernations++;
        if (root) {
          root._hibernated = true;
          this._sleeping.set(session, root);
        }
        this._pending.set(session, blob);
        const done = () => this._pending.get(session) === blob && this._pending.delete(session);
        const w = this._store.set(key, blob);
        if (w && typeof w.then === 'function') w.then(done, (e) => platform.log('hibernation store:', e));
        else done();
      };
      if (actor) {
        if (actor._live && actor._status === 'active' && !actor._hibernated) one(actor._session);
      } else {
        for (const session of native.sweep(this._host, 1 << 30)) one(session);
      }
      return n;
    }

    /* load an actor's blob (it has work, or a migration wants it) */
    _wake(session) {
      if (this._waking.has(session) || !this._store) return; // already being loaded
      this._waking.add(session);
      const key = String(session);
      const finish = (blob) => {
        this._do(() => {
          this._waking.delete(session);
          if (this._stopped) return;
          if (!blob || !native.wake(this._host, session, blob)) {
            platform.log(`tinyactors: cannot wake actor ${key} (no blob in the store)`);
            return;
          }
          this._stats.wakes++;
          this._pending.delete(session);
          const root = this._sleeping.get(session);
          if (root) {
            root._hibernated = false;
            this._sleeping.delete(session);
          }
          const d = this._store.delete(key);
          if (d && typeof d.then === 'function') d.catch(() => {});
          this._dirty = true;
          this._pump();
        });
      };
      const blob = this._pending.get(session) || this._store.get(key);
      if (blob && typeof blob.then === 'function')
        blob.then(finish, (e) => {
          this._waking.delete(session);
          platform.log('hibernation store:', e);
        });
      else finish(blob);
    }

    /** The actor registered with `createActor(logic, { systemID })`. */
    get(systemID) {
      return this._ids.get(systemID);
    }

    _now() {
      return this.options.clock ? this.options.clock() : platform.now() - this._t0;
    }

    /** Run `fn` when this system's clock passes `at` (system time); returns a
     * cancel function. Without a `clock` option that is a platform timer; with
     * one, the callback waits for flush() — time moves only there. Used for
     * link resumption, whose window follows the router's queue TTL. */
    _at(at, fn) {
      if (!this.options.clock) {
        const t = platform.setTimer(fn, Math.max(0, at - this._now()));
        if (t && typeof t.unref === 'function') t.unref();
        return () => platform.clearTimer(t);
      }
      const timers = (this._clockTimers ||= []);
      const entry = { at, fn };
      timers.push(entry);
      return () => {
        const i = timers.indexOf(entry);
        if (i >= 0) timers.splice(i, 1);
      };
    }

    _expire() { /* run the clock timers that are due (with a `clock` option) */
      const timers = this._clockTimers;
      if (!timers || !timers.length) return;
      const now = this._now();
      const due = [];
      for (let i = timers.length - 1; i >= 0; i--) if (timers[i].at <= now) due.push(timers.splice(i, 1)[0]);
      for (let i = due.length - 1; i >= 0; i--) due[i].fn();
    }

    /**
     * Run everything that is due now — events, eventless chains, timers whose
     * time has come on the system's clock — until the system is idle (or
     * `maxSlices` slices ran). With a `clock` option this is how time moves:
     * advance the clock, then flush(). Returns true when idle.
     */
    flush(maxSlices = 100000) {
      if (this._running || this._stopped) return false;
      for (let i = 0; i < maxSlices; i++) {
        if (this._deadline >= 0 && this._deadline <= this._now()) this._dirty = true;
        this._expire();
        if (!this._dirty && !this._yielded && !this._queue.length) return true;
        this._pump();
        platform.cancelDefer(this._immediate);
        this._immediate = null;
      }
      return false;
    }

    _resolve(chartName, texts) {
      const compiled = this._sources.get(chartName.split('#')[0]);
      const sys = this._api;
      return texts.map((t) => {
        const e = compiled && t !== undefined ? compiled.registry.get(norm(t)) : undefined;
        if (!e) return undefined;
        const f = e.fn;
        return (args, $) => {
          args.system = sys;
          if ($ && $.self) args.self = this.ref($.self);
          return f(args, $);
        };
      });
    }

    /** Link `machine` (and every machine not linked yet) into a new segment. */
    _link(machine) {
      const hit = this._linked.get(machine);
      if (hit) return hit;
      if (machine.image) {
        this._linkImage(machine.image);
        return this._linked.get(machine);
      }
      const superseded = (m) => this._current(m) !== m;
      const batch = [
        machine,
        ...[...(this === defaultSystem ? registry : [])].filter((m) => m !== machine && !this._linked.has(m) && !superseded(m)),
      ];
      const sources = [];
      const names = new Map();
      for (const m of batch) {
        let compiled;
        try {
          compiled = m._compile();
        } catch (e) {
          if (m === machine) throw e;
          continue; // another machine's problem surfaces when it is started
        }
        let name = m.id.replace(/[#\s]/g, '_') || 'machine';
        for (let n = 2; this._sources.has(name) || names.has(name); n++) name = `${m.id.replace(/[#\s]/g, '_')}-${n}`;
        names.set(name, m);
        sources.push({ name, xml: compiled.xml, compiled, machine: m });
        /* SCXML documents: the documents their <invoke src> names */
        for (const d of compiled.sources || []) {
          if (names.has(d.name)) continue;
          names.set(d.name, null);
          sources.push({ name: d.name, xml: d.xml });
        }
      }
      const files = sources.flatMap((s) => (s.compiled && s.compiled.files) || []);
      const image = native.compile(
        sources.map(({ name, xml }) => ({ name, xml })),
        files,
      );
      for (const s of sources) if (s.compiled) this._sources.set(s.name, s.compiled);
      native.advance(this._host, this._now());
      const segment = native.addImage(this._host, image);
      this._images[segment] = image;
      this._segInfo[segment] = {
        kind: 'sources',
        files,
        sources: sources.map(({ name, xml, machine: m }) => ({
          name,
          xml,
          logic: !m ? null : m.scxml ? { kind: 'scxml', id: m.id, xml: m.xml, documents: m.documents } : { kind: 'machine', id: m.id },
        })),
      };
      for (const s of sources) {
        if (!s.machine) continue;
        const chart = native.chartIndex(image, s.name);
        this._linked.set(s.machine, { segment, chart });
        this._byChart.set(`${segment}:${chart}`, s.machine);
      }
      return this._linked.get(machine);
    }

    /** The newest version of a machine (see reload()). */
    _current(machine) {
      const v = this._versions.get(machine.id);
      return v ? v.machine : machine;
    }

    /**
     * Hot reload (CGI style): `machine` becomes the current version of the
     * machines with its id. Actors started afterwards run it; running actors
     * stay on their version until they finish or migrate() moves them. A
     * version whose actors are all gone is freed. Returns the version number.
     */
    reload(machine) {
      if (!(machine instanceof StateMachine) && !(machine instanceof SCXMLLogic)) throw new TypeError('reload() expects a machine (createMachine) or an SCXML document (fromSCXML)');
      if (this._stopped) throw new Error('the system was stopped');
      const prev = this._versions.get(machine.id);
      const version = (prev ? prev.version : 1) + 1;
      this._versions.set(machine.id, { version, machine });
      if (!this._linked.has(machine)) {
        try {
          this._link(machine); // compile errors surface here
        } catch (e) {
          if (prev) this._versions.set(machine.id, prev);
          else this._versions.delete(machine.id);
          throw e;
        }
      }
      return version;
    }

    /**
     * Live migration: move running actors to the current version of their
     * machine at their next stable point (a job of the native host layer,
     * advanced in bounded steps as the system runs). States are matched by
     * id, history and context are kept, the session (address) stays the same,
     * and each actor receives `error.platform.upgrade` with the previous
     * context as `event.data`. `target`: an actor, a machine (its actors), or
     * nothing (every actor on an old version). Busy actors move at their next
     * stable point; hibernated ones are loaded first (`wake`, default: when
     * the system has a hibernation store) or move when they wake; `timeoutMs`
     * ends the job with whoever is left on its version. Returns
     * `{ migrated, pending, done }`: `migrated` moved during the call,
     * `pending` (computed when read) still on old versions, `done` resolves
     * with `{ migrated, failed, pending }` once the moved actors processed
     * their upgrade.
     */
    migrate(target, options = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const { wake = !!this._store, timeoutMs } = options;
      if (wake && !this._store) throw new Error('migrate({ wake: true }) needs a hibernation store');
      const single = target instanceof Actor ? target : null;
      // only machines that were reloaded have older versions
      const ids = single ? [single.logic.id] : target ? [target.id] : [...this._versions.keys()];
      const wanted = (a) => ids.includes(a.logic.id) && a._live && a._status === 'active' && this._current(a.logic) !== a.logic;
      const jobs = [];
      for (const id of ids) {
        if (!this._versions.has(id) || (single && !wanted(single))) continue;
        const job = native.migrate(this._host, id, single ? single._session : 0, wake, timeoutMs === undefined ? 0 : Math.max(1, timeoutMs));
        const entry = { moved: [], result: null };
        entry.done = new Promise((resolve) => (entry.resolve = resolve));
        this._jobs.set(job, entry);
        jobs.push(entry);
      }
      this._dirty = true; // jobs advance inside native.run
      if (!this._running) this._pump(); // the first step, and the upgrade events
      const migrated = jobs.flatMap((j) => j.moved);
      const done = Promise.all(jobs.map((j) => j.done)).then((rs) => ({
        migrated: rs.reduce((n, r) => n + r.migrated, 0),
        failed: rs.reduce((n, r) => n + r.failed, 0),
        pending: rs.flatMap((r) => r.pending),
      }));
      const system = this;
      return {
        migrated,
        done,
        /* computed when read (a walk over the actors) */
        get pending() {
          return single ? (wanted(single) ? [single] : []) : [...system._actors.values()].filter(wanted);
        },
      };
    }

    /* Calls into the native system are never nested: work requested while it
     * runs (e.g. actor.send() inside an action) is queued and done afterwards. */
    _do(fn) {
      if (this._stopped) return;
      if (this._running) this._queue.push(fn);
      else fn();
    }

    /** Stop the whole system: every actor stops, links close, timers end and
     * the native memory is freed. The system cannot be used afterwards. */
    stop() {
      if (this._stopped || this._running) return;
      for (const c of [...this._links]) c.disconnect();
      for (const a of [...this._actors.values()]) a._halt();
      this._stopped = true;
      platform.clearTimer(this._timer);
      platform.clearTimer(this._sweepTimer);
      platform.cancelDefer(this._immediate);
      this._clockTimers = null;
      if (this._traceSink || this._exporter) {
        const { traces, logs } = native.traceEnd(this._host); /* open spans end, incomplete */
        try {
          this._traces(traces, logs);
        } catch (e) {
          platform.log(e);
        }
        if (this._exporter) this._exporter.flush();
      }
      native.destroy(this._host);
      this._actors.clear();
      for (const [, j] of this._jobs) j.resolve({ migrated: 0, failed: 0, pending: [] });
      this._jobs.clear();
      for (const resolve of this._settle.splice(0)) resolve();
      this._ids.clear();
      this._observed.clear();
    }

    _spawn(actor) {
      actor.logic = this._current(actor.logic); // new actors run the newest version
      actor._chart = actor.logic._compile().chart;
      const { segment, chart } = this._link(actor.logic);
      native.advance(this._host, this._now());
      const logic = actor.logic;
      const session = logic.scxml
        ? native.spawn(this._host, segment, chart, undefined, actor.options.input || {}, actor.options.systemID, actor.options.traceparent) /* <data> overrides */
        : native.spawn(this._host, segment, chart, actor.options.input, undefined, actor.options.systemID, actor.options.traceparent);
      actor._session = session;
      actor._serial = session;
      actor._live = true;
      this._actors.set(session, actor);
      if (actor.options.systemID) this._ids.set(actor.options.systemID, actor);
      this._dirty = true;
    }

    _send(actor, event, traceparent) {
      native.send(this._host, actor._session, event.type, actor.logic.scxml ? actor.logic._eventData(event) : event, traceparent);
      this._dirty = true;
    }

    /** Send many events at once (one native call; batched per segment in C).
     * `items`: [actor, event] or [actor, event, { traceparent }]. Returns how
     * many were delivered. */
    sendMany(items) {
      const sessions = [];
      const types = [];
      const events = [];
      const traceparents = [];
      for (const [actor, ev, options] of items) {
        const event = typeof ev === 'string' ? { type: ev } : ev;
        if (!(actor instanceof Actor) || !actor._live || actor._status !== 'active') continue;
        sessions.push(actor._session);
        types.push(event.type);
        events.push(actor.logic.scxml ? actor.logic._eventData(event) : event);
        traceparents.push((options && options.traceparent) || '');
      }
      let n = 0;
      this._do(() => {
        n = native.sendMany(this._host, sessions, types, events, traceparents);
        this._dirty = true;
        this._pump();
      });
      return n;
    }

    _terminate(actor) {
      native.terminate(this._host, actor._session); // the session exits on the next run
      this._forget(actor);
      this._dirty = true;
    }

    _forget(actor) {
      if (!actor._live) return;
      actor._live = false;
      this._actors.delete(actor._session);
      if (actor.options.systemID && this._ids.get(actor.options.systemID) === actor) this._ids.delete(actor.options.systemID);
      this._observed.delete(actor);
    }

    _pump() {
      if (this._running || this._stopped) return;
      platform.cancelDefer(this._immediate);
      this._immediate = null;
      native.advance(this._host, this._now());
      if (this._yielded) {
        this._yielded = false;
        this._dirty = true;
      }
      const started = platform.now();
      for (let round = 0; round < 16; round++) {
        let ran = false;
        if (this._dirty) {
          this._dirty = false;
          ran = true;
          this._running = true;
          let r;
          const t0 = platform.now();
          try {
            r = native.run(this._host, this._budget);
          } finally {
            this._running = false;
          }
          if (r.status === 'yielded') {
            const dt = platform.now() - t0;
            if (dt > SLICE_MS) this._budget = Math.max(500, Math.floor(this._budget / 2));
            else if (dt < SLICE_MS / 4) this._budget = Math.min(200000, Math.floor(this._budget * 1.5));
          }
          this._version++;
          this._handle(r);
          if (r.traces || r.traceLogs) this._traces(r.traces, r.traceLogs);
          this._deadline = r.deadline;
          if (r.status !== 'idle') this._yielded = true;
        }
        const queued = this._queue.splice(0);
        for (const fn of queued) fn();
        if (!ran && !queued.length) break;
        if (platform.now() - started > SLICE_MS) break; // the rest in the next pump (deferred below)
      }
      if (this._settle.length && !this._yielded && !this._dirty) for (const resolve of this._settle.splice(0)) resolve();
      for (const actor of [...this._observed]) actor._notify();
      if (this._yielded || this._dirty) this._immediate = platform.defer(() => this._pump());
      this._schedule();
    }

    _schedule() {
      platform.clearTimer(this._timer);
      this._timer = null;
      if (this._deadline < 0 || this.options.clock) return; /* own clock: flush() moves time */
      this._timer = platform.setTimer(() => {
        this._timer = null;
        if (this._deadline >= 0 && this._deadline <= this._now() + 1) this._dirty = true;
        this._pump();
      }, Math.max(0, this._deadline - this._now()));
    }

    _handle(r) {
      /* (native.run lists only what happened) */
      for (const l of r.logs || NONE) {
        const a = this._actors.get(l.session);
        const logger = (a && a.options.logger) || this.options.logger || platform.log;
        if (l.label) logger(l.label, l.message);
        else logger(l.message);
      }
      for (const e of r.errors || NONE) {
        const a = this._actors.get(e.session);
        if (a) a._fail(e.error);
      }
      const ended = new Map((r.ended || NONE).map((g) => [g.session, g]));
      for (const d of r.done || NONE) {
        const a = this._actors.get(d.session);
        if (!a) continue;
        const g = ended.get(d.session);
        a._finish(d.state, d.output, g && g.context, g && g.config);
      }
      for (const p of r.peers || NONE) {
        const c = this._peers.get(p.peer);
        if (c) c.envelope(p);
      }
      for (const e of r.peerErrors || NONE) {
        const c = this._peers.get(e.peer);
        if (c) c.post({ t: 'err', origin: e.origin, msgid: e.msgid, sendid: e.sendid, reason: REASONS[e.reason] || 'unreachable' });
      }
      for (const c of this._links) c.sync();
      for (const session of r.wakes || NONE) this._wake(session);
      for (const q of r.requests || NONE) {
        // an invoke whose src is not a chart of its image: look it up by source name
        const image = this._images[q.segment];
        const chart = image ? native.chartIndex(image, q.src.replace(/^(file:(\/\/)?|\.\/)/, '')) : -1;
        native.resume(this._host, q.segment, q.token, chart);
        this._dirty = true;
      }
      for (const m of r.migrated || NONE) {
        const a = this._actors.get(m.session);
        const machine = this._byChart.get(`${m.segment}:${m.chart}`);
        if (a && machine) {
          a.logic = machine;
          a._chart = machine._compile().chart;
          a._snapVersion = -1;
        }
        const job = this._jobs.get(m.job);
        if (job && a) job.moved.push(a);
      }
      for (const j of r.jobs || NONE) {
        const job = this._jobs.get(j.job);
        if (!job) continue;
        this._jobs.delete(j.job);
        const pending = j.pending.map((s) => this._actors.get(s)).filter(Boolean);
        // settled once the system is idle: the upgrade events have been processed
        this._settle.push(() => job.resolve({ migrated: j.migrated, failed: j.failed, pending }));
      }
      for (const segment of r.freed || NONE) {
        // a retired version: its machines link anew if they are started again
        for (const [m, l] of this._linked) if (l.segment === segment) this._linked.delete(m);
        for (const k of this._byChart.keys()) if (k.startsWith(`${segment}:`)) this._byChart.delete(k);
        this._images[segment] = undefined;
      }
    }

    /** Native resources in use, per live version (segment). */
    stats() {
      const st = native.stats(this._host);
      return st.versions.map((v) => ({
        maxActors: v.maxActors,
        actors: v.actors,
        hibernated: v.hibernated,
        sessions: st.sessions,
        payloads: st.payloads,
        ...this._stats,
      }));
    }
  }

  let defaultSystem = null;
  function getDefaultSystem() {
    if (!defaultSystem) defaultSystem = new System();
    return defaultSystem;
  }

  /** A new, empty actor system. `createActor(machine, { system })` starts actors in it. */
  function createSystem(options) {
    return new System(options);
  }

  /* ------------------------------------------------------------------- actors */

  function stateValue(node, active) {
    const kids = node.children.filter((k) => active.has(k.id) && k.type !== 'history');
    const leaf = (k) => !k.children.some((c) => c.type !== 'history');
    if (node.type === 'parallel') {
      const out = {};
      for (const k of kids) out[k.key] = leaf(k) ? {} : stateValue(k, active);
      return out;
    }
    const k = kids[0];
    if (!k) return {};
    return leaf(k) ? k.key : { [k.key]: stateValue(k, active) };
  }

  function flattenValue(v, prefix) {
    if (typeof v === 'string') return [prefix ? `${prefix}.${v}` : v];
    const out = [];
    for (const [k, sub] of Object.entries(v)) {
      const p = prefix ? `${prefix}.${k}` : k;
      out.push(p);
      out.push(...flattenValue(sub, p).filter((x) => x !== p));
    }
    return out;
  }

  class Actor {
    constructor(logic, options = {}) {
      this.logic = logic;
      this.options = options;
      this.id = options.id || logic.id;
      this.system = options.system || getDefaultSystem();
      this._observers = new Set();
      this._status = 'active';
      this._started = false;
      this._live = false; /* spawned and not finished */
      this._session = 0; /* its session id: names it for life (also after migration) */
      this._serial = 0;
      this._snapshot = null;
      this._snapVersion = -1;
      this._lastNotified = null;
      this._output = undefined;
      this._error = undefined;
      this._final = null; /* { ids, context } once done */
      this._hibernated = false;
    }

    /** True while the actor is hibernated (unloaded to the system's store). */
    get hibernated() {
      return this._hibernated;
    }

    /** This actor's address (usable from other systems across a link). */
    get address() {
      return this._live ? addressOf(this) : undefined;
    }

    start() {
      if (this._started) return this;
      this._started = true;
      this._chart = this.logic._compile().chart;
      this.system._do(() => {
        this.system._spawn(this);
        this.system._pump();
      });
      return this;
    }

    /** `options.traceparent`: the step it starts continues that trace (W3C Trace Context). */
    send(event, options = {}) {
      if (typeof event === 'string') event = { type: event };
      if (!event || typeof event.type !== 'string') throw new TypeError('events must be objects with a string "type"');
      if (!this._started || this._status !== 'active') return;
      this.system._do(() => {
        if (this._status !== 'active') return;
        this.system._send(this, event, options.traceparent);
        this.system._pump();
      });
    }

    _fail(error) {
      this._error = error;
      for (const o of this._observers) o.error && o.error(error);
    }

    _finish(state, output, context, config) {
      this._status = 'done';
      this._output = output;
      const ids = config && state === this._chart.doneID ? config : [state];
      this._final = { ids, context };
      this.system._forget(this);
      this._snapVersion = -1;
      const snap = this.getSnapshot();
      for (const o of this._observers) o.next && o.next(snap);
      for (const o of this._observers) o.complete && o.complete();
      this._observers.clear();
    }

    _notify() {
      if (!this._observers.size || this._status !== 'active') return;
      const s = this.getSnapshot();
      const key = JSON.stringify(s.value);
      const last = this._lastNotified;
      if (last && last.key === key && last.context === s.context) return;
      this._lastNotified = { key, context: s.context };
      for (const o of this._observers) o.next && o.next(s);
    }

    _makeSnapshot() {
      let ids;
      let context;
      if (this._final) {
        ids = this._final.ids;
        context = this._final.context;
      } else if (this._live && this._status === 'active') {
        ids = native.config(this.system._host, this._session);
        context = native.context(this.system._host, this._session);
      } else {
        return this._snapshot;
      }
      const active = new Set(ids);
      const chart = this._chart;
      const value = active.size ? stateValue(chart.root, active) : {};
      const { _status: status, _output: output, _error: error } = this;
      const paths = new Set(flattenValue(value, ''));
      return {
        value,
        context,
        status,
        output,
        error,
        matches(v) {
          return flattenValue(v, '').every((p) => paths.has(p));
        },
        hasTag(tag) {
          for (const p of paths) {
            const n = chart.nodes.get(p);
            if (n && n.tags.includes(tag)) return true;
          }
          return false;
        },
        toJSON() {
          return { value, context, status, output };
        },
      };
    }

    getSnapshot() {
      if (!this._started) return { value: {}, context: undefined, status: 'active', matches: () => false, hasTag: () => false };
      const frozen = this._status !== 'active';
      if (this._hibernated && this._snapshot) return this._snapshot;
      if (!frozen && this._snapVersion !== this.system._version) {
        this._snapshot = this._makeSnapshot();
        this._snapVersion = this.system._version;
      } else if (this._snapVersion === -1) {
        this._snapshot = this._makeSnapshot();
        this._snapVersion = this.system._version;
      }
      return this._snapshot;
    }

    subscribe(nextOrObserver, error, complete) {
      const o = typeof nextOrObserver === 'function' ? { next: nextOrObserver, error, complete } : nextOrObserver;
      this._observers.add(o);
      if (this._status === 'active') {
        this.system._observed.add(this);
        const s = this.getSnapshot();
        if (s && s.value) this._lastNotified = { key: JSON.stringify(s.value), context: s.context };
      }
      return {
        unsubscribe: () => {
          this._observers.delete(o);
          if (!this._observers.size) this.system._observed.delete(this);
        },
      };
    }

    stop() {
      if (this._halt())
        this.system._do(() => {
          this.system._terminate(this);
          this.system._pump();
        });
      return this;
    }

    /* the JS side of stopping: freeze the snapshot, complete observers */
    _halt() {
      let was = false;
      if (this._status === 'active' && this._started) {
        was = true;
        if (!this._hibernated) this._snapshot = this._makeSnapshot();
        this._status = 'stopped';
        if (this._snapshot) this._snapshot = { ...this._snapshot, status: 'stopped' };
        this._snapVersion = this.system._version;
      }
      for (const o of this._observers) o.complete && o.complete();
      this._observers.clear();
      this.system._observed.delete(this);
      return was;
    }
  }

  function createActor(logic, options) {
    return new Actor(logic, options);
  }

  /* ------------------------------------------------------------------- links
   * A link carries envelopes between two systems over a WebSocket-like
   * transport, speaking the link protocol v1 (docs/protocol.md, "Link
   * protocol"): hello {v, id, reach, caps, cred}, ev (an envelope), err,
   * ping/pong. Routing is the native router's: each connection is one router
   * peer, and this code only moves frames between it and the wire. The client
   * side of a link is an actor whose state is the connection status. A
   * transport is WebSocket-like (one text per message) or a byte stream
   * (length-prefixed frames, streamTransport below). */
  const LINK_V = 1;
  const PEER_TRUSTED = 1, PEER_QUEUE = 2;
  const REASONS = ['', 'unknown-target', 'unreachable', 'denied', 'expired', 'hops', 'malformed'];
  const str = (v) => (typeof v === 'string' ? v : '');
  const parseFrame = (text) => {
    try {
      const m = JSON.parse(text);
      return m && typeof m === 'object' && typeof m.t === 'string' ? m : null;
    } catch {
      return null;
    }
  };
  /* what a hello announces: valid system ids only */
  const reachOf = (msg) => (Array.isArray(msg.reach) ? msg.reach.filter(systemIDValid) : []);

  /* what both sides of a link share: frames, envelopes, values on the wire */
  class PeerConn {
    constructor(system, options) {
      this.system = system;
      this.options = options;
      this.ws = null;
      this.open = false;
      this.peer = 0; /* the router's handle */
      this.remoteID = null;
      this.reach = [];
      this.announce = !!options.announce;
      this.announced = null;
      this.profile = 'neutral'; /* "js" once both hellos announced it */
      this.maxFrame = options.maxFrame || FRAME_MAX; /* what we accept, announced in our hello */
      this.peerMax = FRAME_MAX; /* what the peer accepts (its hello's caps.max_frame) */
    }
    /* the value encoding both sides understand (docs/protocol.md, "Link protocol") */
    negotiate(msg) {
      const caps = msg.caps && typeof msg.caps === 'object' ? msg.caps : {};
      this.profile = caps.profile === 'js' && this.options.profile !== 'neutral' ? 'js' : 'neutral';
      this.peerMax = Number.isInteger(caps.max_frame) && caps.max_frame > 0 ? caps.max_frame : FRAME_MAX;
    }
    send(msg) {
      this.ws.send(JSON.stringify(msg));
    }
    hello() {
      const reach = this.announce ? this.system._reachFor(this) : [];
      const key = reach.join(' ');
      if (this.announced === key) return;
      this.announced = key;
      const profile = this.options.profile === 'neutral' ? 'neutral' : 'js';
      const msg = { t: 'hello', v: LINK_V, id: this.system.id, reach, caps: { profile, ordered: true, acks: false, max_frame: this.maxFrame } };
      if (this.options.credential !== undefined) msg.cred = this.options.credential;
      this.send(msg);
    }
    /* a TA_HOST_EFFECT_PEER effect: the envelope goes out on the wire */
    envelope(p) {
      const msg = { t: 'ev', target: p.target, name: p.name, origin: p.origin, msgid: p.msgid, hops: p.hops };
      const fail = (reason) => this.system._fromPeer(this.peer, { t: 'err', origin: p.origin, msgid: p.msgid, sendid: p.sendid, reason });
      if (p.sendid) msg.sendid = p.sendid;
      if (p.traceparent) msg.traceparent = p.traceparent;
      let text;
      try {
        if (p.data !== undefined) msg.data = this.profile === 'js' ? encodeTagged(p.data) : encodeWire(p.data);
        text = JSON.stringify(msg);
      } catch {
        return fail('malformed'); /* not representable (a cycle on a neutral link, a function) */
      }
      /* the peer refuses frames above its max_frame: fail it here, where the origin is known */
      if (text.length * 3 > this.peerMax && utf8.encode(text).length > this.peerMax) return fail('malformed');
      /* the connection went away under the router's feet: it is undeliverable */
      if (!this.post(msg, text)) fail('unreachable');
    }
    post(msg, text = JSON.stringify(msg)) {
      if (!this.open || !this.ws) return false;
      this.ws.send(text);
      return true;
    }
    decode(data) {
      return this.profile === 'js' ? decodeTagged(data, this.system) : decodeWire(data, this.system);
    }
    /* frames every connection handles alike; false: not one of them */
    common(ws, msg) {
      if (msg.t === 'hello') return false;
      if (msg.t === 'ping') ws.send(JSON.stringify({ t: 'pong', at: msg.at }));
      else if ((msg.t === 'ev' || msg.t === 'err') && this.peer && this.remoteID) this.system._fromPeer(this.peer, msg);
      return true; /* pong is the caller's; ack and unknown types are ignored */
    }
  }

  const linkConns = new Map();
  let linkSeq = 0;
  const linkMachine = createMachine(
    {
      id: 'link',
      initial: 'connecting',
      context: ({ input }) => ({ link: input.link, url: input.url, attempts: 0, rtt: null, queued: 0, openedAt: null, peer: null, error: null }),
      on: {
        CLOSE: '.closed',
        QUEUED: { actions: assign({ queued: ({ event }) => event.n }) },
      },
      states: {
        connecting: {
          entry: 'connect',
          on: {
            OPENED: { target: 'open', actions: assign(({ event }) => ({ attempts: 0, openedAt: event.at, peer: event.peer, error: null })) },
            DROPPED: { target: 'reconnecting', actions: assign(({ context, event }) => ({ attempts: context.attempts + 1, error: event.error || null })) },
          },
        },
        open: {
          on: {
            RTT: { actions: assign({ rtt: ({ event }) => event.ms }) },
            DROPPED: { target: 'reconnecting', actions: assign(({ event }) => ({ attempts: 1, openedAt: null, rtt: null, error: event.error || null })) },
          },
        },
        reconnecting: { after: { backoff: 'connecting' } },
        closed: { type: 'final', entry: 'disconnect' },
      },
    },
    {
      actions: {
        connect: ({ context }) => linkConns.get(context.link) && linkConns.get(context.link).connect(),
        disconnect: ({ context }) => linkConns.get(context.link) && linkConns.get(context.link).disconnect(),
      },
      delays: { backoff: ({ context }) => Math.min(30000, 250 * 2 ** Math.min(context.attempts - 1, 7)) },
    },
  );
  registry.delete(linkMachine); /* linked on demand, not into every default image */

  class LinkConn extends PeerConn {
    constructor(system, options) {
      super(system, options);
      this.id = ++linkSeq;
      this.alias = options.name || 'remote';
      if (!systemIDValid(this.alias)) throw new TypeError(`createLink: name "${this.alias}" is not a system id`);
      this.closed = false;
      this.pinger = null;
      this.actor = null;
      this.queued = 0;
      const queue = options.maxQueue !== 0;
      const flags = (options.trusted === false ? 0 : PEER_TRUSTED) | (queue ? PEER_QUEUE : 0);
      this.peer = native.peerAdd(system._host, this.alias, [], flags, options.maxQueue || 1000, options.queueTTLMs || 0);
      if (!this.peer) throw new Error(`createLink: cannot add a peer "${this.alias}"`);
      native.peerUp(system._host, this.peer, false); /* not connected yet: envelopes queue */
      linkConns.set(this.id, this);
      system._attach(this);
      system._links.add(this);
    }
    connect() {
      if (this.closed) return;
      let ws;
      try {
        ws = this.options.transport ? asTransport(this.options.transport(), this.maxFrame) : new WebSocket(this.options.url);
      } catch (e) {
        platform.defer(() => this.dropped(ws, String(e && e.message ? e.message : e)));
        return;
      }
      this.ws = ws;
      this.announced = null;
      ws.onopen = () => this.hello();
      ws.onmessage = (ev) => this.message(ws, String(ev.data));
      ws.onclose = () => this.dropped(ws, 'connection closed');
      ws.onerror = () => {}; /* followed by onclose */
    }
    message(ws, text) {
      if (ws !== this.ws) return;
      const msg = parseFrame(text);
      if (!msg || this.common(ws, msg)) {
        if (msg && msg.t === 'pong') {
          const ms = Math.round((this.system._now() - msg.at) * 10) / 10;
          native.peerRtt(this.system._host, this.peer, ms);
          this.actor.send({ type: 'RTT', ms });
        }
        return;
      }
      /* hello: the server's id and what it reaches (repeated when that changes) */
      if (msg.v !== LINK_V || !systemIDValid(msg.id)) {
        ws.close();
        return;
      }
      const first = !this.open;
      this.remoteID = msg.id;
      this.reach = reachOf(msg);
      this.negotiate(msg);
      this.open = true;
      this.system._peerHello(this, [msg.id, ...this.reach]);
      if (!first) return;
      this.actor.send({ type: 'OPENED', peer: msg.id, at: this.system._now() });
      const every = this.options.pingMs === undefined ? 5000 : this.options.pingMs;
      if (every > 0) {
        const ping = () => ws.send(JSON.stringify({ t: 'ping', at: this.system._now() }));
        ping();
        this.pinger = setInterval(ping, every);
        if (this.pinger.unref) this.pinger.unref();
      }
    }
    /* our name for the server (the link's alias) becomes its own id on the wire */
    envelope(p) {
      const a = this.remoteID && this.alias !== this.remoteID ? parseAddress(p.target) : null;
      super.envelope(a && a.system === this.alias ? { ...p, target: `tinyactors://${this.remoteID}/${a.local}` } : p);
    }
    /* the router's queue for this peer, reported to the link actor */
    sync() {
      const st = native.peerStatus(this.system._host, this.peer);
      const n = st ? st.queued : 0;
      if (n !== this.queued && this.actor) {
        this.queued = n;
        this.actor.send({ type: 'QUEUED', n });
      }
    }
    dropped(ws, error) {
      if (ws !== this.ws) return;
      this.ws = null;
      this.open = false;
      clearInterval(this.pinger);
      this.pinger = null;
      if (this.closed) return;
      this.system._peerDown(this, false);
      this.actor.send({ type: 'DROPPED', error });
    }
    disconnect() {
      if (this.closed) return;
      this.closed = true;
      this.open = false;
      clearInterval(this.pinger);
      const ws = this.ws;
      this.ws = null;
      if (ws) ws.close();
      this.system._links.delete(this);
      linkConns.delete(this.id);
      if (!this.system._stopped) this.system._peerDown(this, true);
    }
  }

  /** Link `system` to another system (e.g. a browser to its server). */
  function createLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createLink(system, options): system expected');
    if (!options.url && !options.transport) throw new TypeError('createLink: { url } or { transport } needed');
    const conn = new LinkConn(system, options);
    const actor = createActor(linkMachine, { system, id: `link:${conn.alias}`, input: { link: conn.id, url: options.url } });
    conn.actor = actor;
    actor.get = (systemID) => system.ref(formatAddress(conn.alias, systemID));
    actor.close = () => actor.send({ type: 'CLOSE' });
    actor.status = () => native.peerStatus(system._host, conn.peer);
    return actor.start();
  }

  /* exports: ['lobby', ...] or { lobby: 'JOIN LEAVE', ... } -> [name, events] */
  function exportsOf(x) {
    if (!x) return [];
    if (Array.isArray(x)) return x.map((n) => [String(n), '']);
    return Object.entries(x).map(([n, ev]) => [n, Array.isArray(ev) ? ev.join(' ') : String(ev || '')]);
  }

  class ServerConn extends PeerConn {
    constructor(system, options, ws, server) {
      super(system, options);
      this.ws = ws;
      this.server = server;
    }
  }

  /** Accept links into `system`: `accept(onConnection)` registers a source of
   * open transports (WebSocket-like or byte streams) and may return a function
   * that stops it. Untrusted peers
   * (the default) may only send to what `exports` lists; trusted ones
   * (`trusted: true`, or `trust(hello)` returning true) may send anything and
   * relay through this system. A peer that reconnects with the same system id
   * within `resumeMs` resumes: what was queued for it meanwhile is delivered.
   * The resumption window runs on the system's clock, like the router's queue
   * TTL — with a `clock` option, flush() moves it. */
  function serveLinks(system, options = {}) {
    if (typeof options.accept !== 'function') throw new TypeError('serveLinks(system, { accept }) needs an accept function here');
    const conns = new Set();
    const byID = new Map(); /* remote system id -> its ServerConn (also while it is away) */
    const expiry = new Map(); /* remote system id -> cancels the resumption of its peer */
    const resumeMs = options.resumeMs === undefined ? 30000 : options.resumeMs;
    const rules = exportsOf(options.exports);
    const trusts = (hello) => (typeof options.trust === 'function' ? !!options.trust(hello) : !!options.trusted);
    const server = { closed: false };
    const forget = (c) => {
      const cancel = expiry.get(c.remoteID);
      if (cancel) cancel();
      expiry.delete(c.remoteID);
      if (byID.get(c.remoteID) === c) byID.delete(c.remoteID);
      system._peerDown(c, true);
    };
    const hello = (c, ws, msg) => {
      if (msg.v !== LINK_V || !systemIDValid(msg.id) || msg.id === system.id) return ws.close();
      c.reach = reachOf(msg);
      c.negotiate(msg);
      if (c.peer) return system._peerHello(c, c.reach); /* a repeated hello: new reach */
      c.remoteID = msg.id;
      const before = byID.get(msg.id);
      if (before) {
        /* the same system again: the newer connection wins and resumes its peer */
        const cancel = expiry.get(msg.id);
        if (cancel) cancel();
        expiry.delete(msg.id);
        if (before.ws && before.ws !== ws) {
          const old = before.ws;
          before.ws = null;
          before.open = false;
          conns.delete(before);
          old.close();
        }
        c.peer = before.peer;
      } else {
        const flags = (trusts(msg) ? PEER_TRUSTED : 0) | (resumeMs > 0 ? PEER_QUEUE : 0);
        c.peer = native.peerAdd(system._host, msg.id, c.reach, flags, options.maxQueue || 1000, resumeMs);
        if (!c.peer) return ws.close();
        for (const [name, events] of rules) native.exportName(system._host, c.peer, name, events);
      }
      byID.set(msg.id, c);
      system._attach(c);
      c.open = true;
      c.hello();
      system._peerHello(c, c.reach);
    };
    const onConnection = (transport) => {
      const ws = asTransport(transport, options.maxFrame || FRAME_MAX);
      if (server.closed) return ws.close();
      const c = new ServerConn(system, options, ws, server);
      conns.add(c);
      ws.onmessage = (ev) => {
        if (c.ws !== ws) return;
        const msg = parseFrame(String(ev.data));
        if (msg && !c.common(ws, msg)) hello(c, ws, msg);
      };
      ws.onclose = () => {
        conns.delete(c);
        if (c.ws !== ws) return; /* replaced by a newer connection */
        c.ws = null;
        c.open = false;
        if (!c.peer || byID.get(c.remoteID) !== c) return;
        if (resumeMs > 0 && !server.closed) {
          system._peerDown(c, false); /* queue for it until it is back or resumeMs passed */
          const id = c.remoteID;
          /* the resumption window on the system's clock (with a `clock`
           * option, forget runs when flush() moves time past it) */
          expiry.set(id, system._at(system._now() + resumeMs, () => byID.get(id) === c && !c.ws && forget(c)));
        } else forget(c);
      };
    };
    const stop = options.accept(onConnection);
    return {
      connections: () => [...conns].filter((c) => c.open).map((c) => c.remoteID),
      status: (id) => (byID.get(id) ? native.peerStatus(system._host, byID.get(id).peer) : null),
      close: async () => {
        server.closed = true;
        for (const c of [...conns]) {
          const ws = c.ws;
          c.ws = null;
          c.open = false;
          if (ws) ws.close();
        }
        conns.clear();
        for (const c of [...byID.values()]) if (!system._stopped) forget(c);
        if (typeof stop === 'function') await stop();
      },
    };
  }

  /* ------------------------------------------------------------- store links
   * A store link connects the system to a database of actors (docs/protocol.md,
   * "Store links": PostgreSQL's tinyactors extension) through SQL — with the
   * caller's client, whatever it is: `query(sql, params) -> Promise<rows>` and
   * optionally `listen(channel, onPayload) -> unlisten`. The database is one
   * router peer: envelopes for it go in with tinyactors.deliver(envelope)
   * (in order, retried until the database takes them: at-least-once, the
   * database drops repeats by msgid); envelopes from it are claimed from its
   * outbox (claim_for, ordered) when it NOTIFYs or every pollMs, handed to the
   * router and acked; errors travel both ways. Values are neutral. */
  class StoreConn extends PeerConn {
    constructor(system, options) {
      super(system, { ...options, announce: options.announce !== false, profile: 'neutral' });
      this.q = options.query;
      this.remoteID = options.id || null;
      this.worker = options.worker || `${system.id}/${randomID()}`;
      this.batch = options.batch || 100;
      this.out = []; /* frames for the database, in order */
      this.peerMax = Infinity; /* rows, not frames */
      this.pumping = false;
      this.draining = false;
      this.again = false;
      this.closed = false;
      this.error = null;
      this.idle = [];
    }
    async start() {
      if (!this.remoteID) {
        const r = await this.q('SELECT tinyactors.system_id() AS id', []);
        this.remoteID = r[0].id;
      }
      if (!systemIDValid(this.remoteID)) throw new TypeError(`createStoreLink: "${this.remoteID}" is not a system id`);
      const flags = (this.options.trusted === false ? 0 : PEER_TRUSTED) | PEER_QUEUE;
      this.peer = native.peerAdd(this.system._host, this.remoteID, [], flags, this.options.maxQueue || 10000, this.options.queueTTLMs || 0);
      if (!this.peer) throw new Error(`createStoreLink: cannot add a peer "${this.remoteID}"`);
      this.system._attach(this);
      this.system._links.add(this);
      this.open = true;
      this.hello(); /* tinyactors.hello(): what we reach is claimed by us */
      this.system._peerHello(this, []);
      if (this.options.listen) {
        this.unlisten = await this.options.listen('tinyactors_outbox', (payload) => {
          if (payload === this.system.id) this.drain();
        });
      }
      const every = this.options.pollMs === undefined ? 1000 : this.options.pollMs;
      if (every > 0) {
        this.poller = platform.setTimer(function tick() {
          this.drain();
          this.poller = platform.setTimer(tick.bind(this), every);
          if (this.poller && this.poller.unref) this.poller.unref();
        }.bind(this), every);
        if (this.poller && this.poller.unref) this.poller.unref();
      }
      await this.drain();
    }
    /* frames become SQL, in order */
    send(msg) {
      this.out.push(msg);
      this.pump();
    }
    post(msg) {
      if (this.closed) return false;
      this.send(msg);
      return true;
    }
    async pump() {
      if (this.pumping) return;
      this.pumping = true;
      try {
        while (this.out.length && !this.closed) {
          const msg = this.out[0];
          let r;
          try {
            r = await this.sql(msg);
          } catch (e) {
            this.failed(e); /* down: keep it, try again later */
            await new Promise((ok) => platform.setTimer(ok, 500));
            continue;
          }
          this.out.shift();
          this.recovered();
          if (msg.t === 'ev' && r !== 'ok' && r !== 'duplicate') {
            this.system._fromPeer(this.peer, { t: 'err', origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: REASONS.includes(r) ? r : 'unreachable' });
          }
        }
      } finally {
        this.pumping = false;
        this.settled();
      }
    }
    async sql(msg) {
      if (msg.t === 'hello') {
        const r = await this.q('SELECT tinyactors.hello($1, $2::text[]) AS id', [msg.id, `{${msg.reach.join(',')}}`]);
        return r[0] && r[0].id;
      }
      if (msg.t === 'ev') {
        const { t, ...env } = msg;
        /* as this system: the database checks the origin against what we announced */
        const r = await this.q('SELECT tinyactors.deliver($1::jsonb, $2) AS r', [JSON.stringify(env), this.system.id]);
        return r[0] && r[0].r;
      }
      if (msg.t === 'err') {
        await this.q('SELECT tinyactors.deliver_error($1::jsonb)', [JSON.stringify({ origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: msg.reason })]);
      }
      return null;
    }
    /* claim what the database has for us (and the systems we announced) */
    async drain() {
      if (this.closed || !this.peer) return;
      if (this.draining) {
        this.again = true;
        return;
      }
      this.draining = true;
      try {
        do {
          this.again = false;
          const rows = await this.q('SELECT id, envelope FROM tinyactors.claim_for($1, $2, $3)', [this.system.id, this.batch, this.worker]);
          for (const row of rows) {
            const env = typeof row.envelope === 'string' ? JSON.parse(row.envelope) : row.envelope;
            this.system._fromPeer(this.peer, { ...env, t: 'ev' });
          }
          if (rows.length) await this.q('SELECT tinyactors.ack(id) FROM unnest($1::bigint[]) AS id', [`{${rows.map((r) => r.id).join(',')}}`]);
          if (rows.length === this.batch) this.again = true;
          this.recovered();
        } while (this.again && !this.closed);
      } catch (e) {
        this.failed(e);
      } finally {
        this.draining = false;
        this.settled();
      }
    }
    failed(e) {
      this.error = String(e && e.message ? e.message : e);
      if (this.open) {
        this.open = false;
        this.system._peerDown(this, false); /* the router queues meanwhile */
      }
    }
    recovered() {
      if (this.open || this.closed) return;
      this.open = true;
      this.error = null;
      this.announced = null;
      this.hello();
      this.system._peerHello(this, []);
    }
    settled() {
      if (!this.pumping && !this.draining && !this.out.length) for (const ok of this.idle.splice(0)) ok();
    }
    sync() {}
    disconnect() {
      if (this.closed) return;
      this.closed = true;
      this.open = false;
      platform.clearTimer(this.poller);
      if (this.unlisten) Promise.resolve(this.unlisten()).catch(() => {});
      this.system._links.delete(this);
      if (!this.system._stopped) this.system._peerDown(this, true);
      this.settled();
    }
  }

  /** Connect `system` to a database of actors (PostgreSQL + the tinyactors
   * extension) through your own client: `query(sql, params)` resolving to rows,
   * optionally `listen(channel, onPayload)`. */
  function createStoreLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createStoreLink(system, options): system expected');
    if (typeof options.query !== 'function') throw new TypeError('createStoreLink: { query(sql, params) } is needed');
    const conn = new StoreConn(system, options);
    const ready = conn.start();
    ready.catch(() => {});
    return {
      get id() {
        return conn.remoteID;
      },
      ready,
      /** Claim and deliver what the database has for us now. */
      drain: () => ready.then(() => conn.drain()),
      /** Resolves once everything handed to the link reached the database and nothing is being claimed. */
      flush: () =>
        ready.then(
          () =>
            new Promise((ok) => {
              conn.idle.push(ok);
              conn.settled();
            }),
        ),
      status: () => (conn.peer ? { ...native.peerStatus(system._host, conn.peer), error: conn.error, pending: conn.out.length } : null),
      close: async () => {
        conn.disconnect();
      },
    };
  }

  return {
    parseAddress,
    formatAddress,
    createStoreLink,
    encodeWire,
    decodeWire,
    createMachine,
    createActor,
    createSystem,
    fromSCXML,
    SCXMLLogic,
    compileSystem,
    loadImage,
    SystemImage,
    setup,
    assign,
    raise,
    sendTo,
    sendParent,
    log,
    cancel,
    stateIn,
    and,
    or,
    not,
    StateMachine,
    Actor,
    System,
    ActorRef,
    createLink,
    serveLinks,
    encodeFrame,
    FrameDecoder,
    createMemoryStore,
    encodeValue,
    decodeValue,
    TRACE_KINDS,
    decodeTraces,
    parseTraceparent,
    formatTraceparent,
    stepSpanID,
  };
}

/* ------------------------------------------------------------------ frames
 * The link protocol over byte streams — TCP, stdio, serial (docs/protocol.md,
 * "Framing"): a 4-byte big-endian length, then that many bytes of UTF-8 JSON.
 * A frame above the receiver's max_frame (default 1 MiB) is refused as
 * malformed: skipped as it passes, never buffered, so the stream stays in
 * step. No platform APIs beyond TextEncoder/TextDecoder: any transport can
 * use the codec. */
export const FRAME_MAX = 1 << 20;
const utf8 = new TextEncoder();
const utf8Strict = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const u32 = (b, i) => ((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]) >>> 0;

/** One frame: the length prefix and the UTF-8 bytes of `text`. */
export function encodeFrame(text) {
  const body = utf8.encode(String(text));
  const out = new Uint8Array(4 + body.length);
  new DataView(out.buffer).setUint32(0, body.length);
  out.set(body, 4);
  return out;
}

/** Bytes in, frames out: push() takes chunks as they arrive (any split) and
 * returns the frames they complete — a string each, or for a refused frame
 * { reason: 'malformed', length, detail }. */
export class FrameDecoder {
  constructor({ maxFrame = FRAME_MAX } = {}) {
    this.maxFrame = maxFrame;
    this.head = new Uint8Array(4); /* a length prefix split across chunks */
    this.headN = 0;
    this.body = null; /* the frame being completed */
    this.bodyN = 0;
    this.skip = 0; /* bytes of a refused frame still to pass */
  }
  push(chunk) {
    const out = [];
    const n = chunk.length;
    let i = 0;
    while (i < n) {
      if (this.skip) {
        const k = Math.min(this.skip, n - i);
        this.skip -= k;
        i += k;
        continue;
      }
      if (!this.body) {
        let len;
        if (!this.headN && n - i >= 4) {
          len = u32(chunk, i);
          i += 4;
        } else {
          this.head[this.headN++] = chunk[i++];
          if (this.headN < 4) continue;
          this.headN = 0;
          len = u32(this.head, 0);
        }
        if (len > this.maxFrame) {
          out.push({ reason: 'malformed', length: len, detail: `a frame of ${len} bytes is above max_frame (${this.maxFrame})` });
          this.skip = len;
          continue;
        }
        if (n - i >= len) {
          out.push(this._text(chunk.subarray(i, i + len)));
          i += len;
          continue;
        }
        this.body = new Uint8Array(len);
        this.bodyN = 0;
      }
      const k = Math.min(this.body.length - this.bodyN, n - i);
      this.body.set(chunk.subarray(i, i + k), this.bodyN);
      this.bodyN += k;
      i += k;
      if (this.bodyN === this.body.length) {
        out.push(this._text(this.body));
        this.body = null;
      }
    }
    return out;
  }
  _text(bytes) {
    try {
      return utf8Strict.decode(bytes);
    } catch {
      return { reason: 'malformed', length: bytes.length, detail: 'a frame that is not UTF-8' };
    }
  }
}

/* A byte stream as a link transport (the WebSocket-like shape links use):
 * a Node-style duplex (a net.Socket) or a { readable, writable } pair of
 * Node-style streams (a child process's stdout/stdin, or process.stdin/stdout
 * inside the child). Anything with send() is a transport already. */
function asTransport(x, maxFrame) {
  return x && typeof x.send !== 'function' && (typeof x.write === 'function' || (x.writable && typeof x.writable.write === 'function'))
    ? streamTransport(x, maxFrame)
    : x;
}

function streamTransport(stream, maxFrame) {
  const pair = typeof stream.write !== 'function';
  const r = pair ? stream.readable : stream;
  const w = pair ? stream.writable : stream;
  const decoder = new FrameDecoder({ maxFrame });
  const t = { onopen: null, onmessage: null, onclose: null, onerror: null, readyState: 0 };
  let done = false;
  const data = (chunk) => {
    for (const f of decoder.push(chunk)) {
      if (done) return;
      if (typeof f === 'string') t.onmessage && t.onmessage({ data: f });
      else t.onerror && t.onerror(f); /* refused (skipped); the stream stays in step */
    }
  };
  const finish = () => {
    if (done) return;
    done = true;
    t.readyState = 3;
    r.off('data', data);
    queueMicrotask(() => t.onclose && t.onclose({}));
  };
  t.send = (text) => {
    if (!done) w.write(encodeFrame(text));
  };
  t.close = () => {
    if (done) return;
    try {
      w.end();
      if (pair && r.destroy) r.destroy();
    } catch {
      /* e.g. process.stdout cannot be ended */
    }
    finish();
  };
  r.on('data', data);
  for (const s of pair ? [r, w] : [r]) for (const ev of ['end', 'close', 'error']) s.on(ev, finish);
  if (r.destroyed || r.readableEnded) finish();
  else if (w.connecting) w.once('connect', () => !done && ((t.readyState = 1), t.onopen && t.onopen({})));
  else
    queueMicrotask(() => {
      if (done) return;
      t.readyState = 1;
      if (t.onopen) t.onopen({});
    });
  return t;
}
