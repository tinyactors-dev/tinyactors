// Torture chamber dashboard: custom elements that clone <template>s, canvas
// charts, and a WebSocket to torture-bench. No dependencies, no build step.

import { shadow } from './sheet.js';

/* ------------------------------------------------------------- helpers */

const $ = (root, sel) => root.querySelector(sel);
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const SERIES = [1, 2, 3, 4, 5, 6].map((i) => `--series-${i}`);

function abbreviate(v) {
  const a = Math.abs(v);
  if (a >= 1e9) return `${(v / 1e9).toFixed(a >= 1e10 ? 0 : 1)}G`;
  if (a >= 1e6) return `${(v / 1e6).toFixed(a >= 1e7 ? 0 : 1)}M`;
  if (a >= 1e3) return `${(v / 1e3).toFixed(a >= 1e4 ? 0 : 1)}k`;
  if (a >= 100 || Number.isInteger(v)) return `${Math.round(v)}`;
  return v.toFixed(a >= 10 ? 1 : 2);
}
function bytes(v) {
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (Math.abs(v) >= 1024 && i < u.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${u[i]}`;
}
const FORMAT = { num: abbreviate, bytes, pct: (v) => `${Math.round(v)}%`, ms: (v) => `${v >= 100 ? Math.round(v) : v.toFixed(1)} ms` };
/** Axis labels: no trailing zeros ("2.50" -> "2.5", "5.0k" -> "5k"). */
const trimZeros = (s) => s.replace(/(\.\d*?)0+(?=\D|$)/, '$1').replace(/\.(?=\D|$)/, '');
/** Precise fallback when the short format repeats a label (0.15 ms and 0.2 ms both "0.2 ms"). */
const PRECISE = { num: (v) => String(+v.toPrecision(3)), ms: (v) => `${+v.toPrecision(3)} ms`, pct: (v) => `${+v.toPrecision(3)}%`, bytes };
/** The top of a y axis split in 4 steps whose labels stay exact: 1, 2, 4, 6, 8 × 10^n
 * (× 1024^k for bytes), so a 5k axis is not labelled 1.3k, 2.5k, 3.8k. */
function axisTop(max, format) {
  if (!(max > 0)) return 4; /* nothing yet, or all zero: 0 1 2 3 4, not 0.25 steps */
  const base = format === 'bytes' ? 1024 ** Math.max(0, Math.floor(Math.log(max) / Math.log(1024))) : 1;
  const x = max / base, p = 10 ** Math.floor(Math.log10(x)), m = x / p;
  const top = (m <= 1 ? 1 : m <= 2 ? 2 : m <= 4 ? 4 : m <= 6 ? 6 : m <= 8 ? 8 : 10) * p;
  return format === 'bytes' && top >= 1000 ? 1024 * base : top * base; /* 1 MB, not 1000 KB */
}
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

/** Base class: clone the element's <template> into its shadow root once. */
class Base extends HTMLElement {
  constructor() {
    super();
    shadow(this); // upgrades nested elements now
    this.refs = {};
    for (const el of this.shadowRoot.querySelectorAll('[data-ref]')) this.refs[el.dataset.ref] = el;
  }
}

/** Render on the next animation frame (coalesced). */
function scheduler(fn) {
  let queued = false;
  return () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      fn();
    });
  };
}

/** Canvas sized to its box at device pixel ratio. */
function fitCanvas(canvas) {
  const r = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: r.width, h: r.height };
}

/* ------------------------------------------------------------ elements */

class TbStatus extends Base {
  static observedAttributes = ['label'];
  attributeChangedCallback() {
    $(this.shadowRoot, '.pill-label').textContent = this.getAttribute('label') || '';
  }
  set(label, tone) {
    this.setAttribute('label', label);
    this.setAttribute('tone', tone);
  }
}

class TbMeter extends Base {
  connectedCallback() {
    $(this.shadowRoot, '.meter-label').textContent = this.getAttribute('label') || '';
  }
  set(used, cap) {
    const pct = cap ? Math.min(100, (used / cap) * 100) : 0;
    $(this.shadowRoot, '.meter-value').textContent = `${abbreviate(used)} / ${abbreviate(cap)}`;
    $(this.shadowRoot, '.meter-fill').style.width = `${pct}%`;
    this.setAttribute('level', pct >= 95 ? 'danger' : pct >= 80 ? 'warn' : 'ok');
  }
}

class TbStat extends Base {
  constructor() {
    super();
    this.values = [];
    this.render = scheduler(() => this.draw());
  }
  configure({ label, unit = '', format = 'num' }) {
    $(this.shadowRoot, '.stat-label').textContent = label;
    $(this.shadowRoot, '.unit').textContent = unit;
    this.format = FORMAT[format];
  }
  set(value, history) {
    $(this.shadowRoot, '.num').textContent = value == null ? '–' : this.format(value);
    this.values = history;
    this.render();
  }
  draw() {
    const canvas = $(this.shadowRoot, '.spark');
    const { ctx, w, h } = fitCanvas(canvas);
    ctx.clearRect(0, 0, w, h);
    const v = this.values;
    if (v.length < 2) return;
    const max = Math.max(...v), min = Math.min(0, ...v);
    const y = (x) => h - 2 - ((x - min) / (max - min || 1)) * (h - 4);
    ctx.beginPath();
    v.forEach((x, i) => ctx[i ? 'lineTo' : 'moveTo']((i / (v.length - 1)) * w, y(x)));
    ctx.strokeStyle = css('--accent');
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }
}

class TbChart extends Base {
  constructor() {
    super();
    this.series = [];
    this.muted = new Set();
    this.window = 120;
    this.hover = null;
    this.render = scheduler(() => this.draw());
    this.plot = $(this.shadowRoot, '.plot');
    this.canvas = $(this.shadowRoot, 'canvas');
    this.readout = $(this.shadowRoot, '.readout');
    this.plot.addEventListener('pointermove', (e) => {
      const r = this.plot.getBoundingClientRect();
      this.hover = e.clientX - r.left;
      this.render();
    });
    this.plot.addEventListener('pointerleave', () => {
      this.hover = null;
      this.render();
    });
    new ResizeObserver(() => this.render()).observe(this.plot);
  }
  configure({ title, unit = '', format = 'num', max }) {
    this.max = max;
    $(this.shadowRoot, '.chart-title').textContent = title;
    $(this.shadowRoot, '.chart-unit').textContent = unit;
    this.formatName = FORMAT[format] ? format : 'num';
    this.format = FORMAT[this.formatName];
  }
  /** series: [{ name, points: [[t, v], ...] }]; now: latest time (s) */
  update(series, now, windowSec) {
    this.window = windowSec;
    this.now = now;
    const names = series.map((s) => s.name).join('\u0000');
    this.series = series;
    if (names !== this.legendNames) this.buildLegend();
    this.render();
  }
  buildLegend() {
    this.legendNames = this.series.map((s) => s.name).join('\u0000');
    const legend = $(this.shadowRoot, '.legend');
    legend.replaceChildren(
      ...this.series.map((s, i) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.setAttribute('aria-pressed', String(!this.muted.has(s.name)));
        const sw = document.createElement('span');
        sw.className = 'swatch';
        sw.style.background = `var(${SERIES[i % SERIES.length]})`;
        b.append(sw, document.createTextNode(s.name));
        b.addEventListener('click', () => {
          if (this.muted.has(s.name)) this.muted.delete(s.name);
          else this.muted.add(s.name);
          b.setAttribute('aria-pressed', String(!this.muted.has(s.name)));
          this.render();
        });
        return b;
      }),
    );
  }
  draw() {
    const { ctx, w, h } = fitCanvas(this.canvas);
    ctx.clearRect(0, 0, w, h);
    const padR = 8, padT = 6, padB = 20;
    const t1 = this.now ?? 0, t0 = t1 - this.window;
    const visible = this.series.map((s, i) => ({ ...s, color: css(SERIES[i % SERIES.length]), dashed: i >= SERIES.length })).filter((s) => !this.muted.has(s.name));
    let max = 0;
    for (const s of visible) for (const [t, v] of s.points) if (t >= t0 && v > max) max = v;
    const top = this.max && max <= this.max ? this.max : axisTop(max * 1.1, this.formatName);
    let labels = [0, 1, 2, 3, 4].map((i) => trimZeros(this.format((top / 4) * i)));
    if (new Set(labels).size < labels.length) labels = [0, 1, 2, 3, 4].map((i) => PRECISE[this.formatName]((top / 4) * i));
    // the y axis is as wide as its widest label ("100 ms", "1.5k")
    ctx.font = `12px ${css('--font-sans')}`;
    let labelW = 0;
    for (const l of labels) labelW = Math.max(labelW, ctx.measureText(l).width);
    const padL = Math.max(36, Math.ceil(labelW) + 12);
    const pw = w - padL - padR, ph = h - padT - padB;
    const X = (t) => padL + ((t - t0) / (t1 - t0 || 1)) * pw;
    const Y = (v) => padT + ph - (v / top) * ph;
    // grid + y ticks
    ctx.fillStyle = css('--text-3');
    ctx.strokeStyle = css('--border');
    ctx.lineWidth = 1;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
      const v = (top / 4) * i, y = Math.round(Y(v)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(w - padR, y);
      ctx.stroke();
      ctx.fillText(labels[i], padL - 8, y);
    }
    // x ticks (relative time), kept inside the plot: 0:00 at the right edge stays readable
    ctx.textBaseline = 'top';
    const step = this.window <= 30 ? 10 : this.window <= 120 ? 30 : 120;
    for (let t = Math.ceil(t0 / step) * step; t <= t1; t += step) {
      if (t < 0) continue;
      const label = clock(t), half = ctx.measureText(label).width / 2;
      const x = Math.min(Math.max(X(t), padL + half), w - padR - half);
      ctx.textAlign = 'center';
      ctx.fillText(label, x, h - padB + 6);
    }
    // lines
    ctx.save();
    ctx.beginPath();
    ctx.rect(padL, 0, pw, h);
    ctx.clip();
    for (const s of visible) {
      const pts = s.points.filter(([t]) => t >= t0 - 1);
      if (!pts.length) continue;
      ctx.beginPath();
      pts.forEach(([t, v], i) => ctx[i ? 'lineTo' : 'moveTo'](X(t), Y(v)));
      ctx.strokeStyle = s.color;
      ctx.lineWidth = 1.5;
      ctx.setLineDash(s.dashed ? [4, 3] : []);
      ctx.stroke();
      if (visible.length === 1) {
        ctx.lineTo(X(pts[pts.length - 1][0]), Y(0));
        ctx.lineTo(X(pts[0][0]), Y(0));
        ctx.closePath();
        ctx.globalAlpha = 0.08;
        ctx.fillStyle = s.color;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
    }
    ctx.restore();
    // hover readout
    if (this.hover == null || this.hover < padL || !visible.length) {
      this.readout.hidden = true;
      return;
    }
    const t = t0 + ((this.hover - padL) / pw) * (t1 - t0);
    ctx.strokeStyle = css('--text-3');
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.moveTo(Math.round(this.hover) + 0.5, padT);
    ctx.lineTo(Math.round(this.hover) + 0.5, padT + ph);
    ctx.stroke();
    const rows = visible.map((s) => {
      let best = null;
      for (const p of s.points) if (!best || Math.abs(p[0] - t) < Math.abs(best[0] - t)) best = p;
      return { name: s.name, color: s.color, v: best ? best[1] : null };
    });
    this.readout.replaceChildren();
    const head = document.createElement('div');
    head.className = 'readout-time';
    head.textContent = clock(Math.max(0, t));
    this.readout.append(head);
    for (const r of rows) {
      const row = document.createElement('div');
      row.className = 'readout-row';
      const name = document.createElement('span');
      const sw = document.createElement('span');
      sw.className = 'swatch';
      sw.style.background = r.color;
      name.append(sw, document.createTextNode(r.name));
      const val = document.createElement('span');
      val.textContent = r.v == null ? '–' : this.format(r.v);
      row.append(name, val);
      this.readout.append(row);
    }
    this.readout.hidden = false;
    const rw = this.readout.offsetWidth;
    const left = this.hover + 12 + rw > w ? this.hover - 12 - rw : this.hover + 12;
    this.readout.style.left = `${Math.max(0, left)}px`;
  }
}

class TbControls extends Base {
  constructor() {
    super();
    this.refs.stream.addEventListener('click', () => this.dispatchEvent(new CustomEvent('stream', { bubbles: true, composed: true })));
    this.refs.sim.addEventListener('click', () => this.dispatchEvent(new CustomEvent('sim', { bubbles: true, composed: true })));
    for (const b of this.refs.window.querySelectorAll('button')) {
      b.addEventListener('click', () => {
        for (const o of this.refs.window.querySelectorAll('button')) o.setAttribute('aria-checked', String(o === b));
        this.dispatchEvent(new CustomEvent('window', { bubbles: true, composed: true, detail: Number(b.dataset.window) }));
      });
    }
  }
  setMutations(list) {
    this.refs.hint.hidden = !list.length;
    this.refs.mutations.replaceChildren(
      ...list.map((m) => {
        const row = document.createElement('div');
        row.className = 'mutation';
        const text = document.createElement('div');
        const title = document.createElement('div');
        title.className = 'mutation-title';
        title.textContent = m.title;
        text.append(title);
        if (m.description) {
          const d = document.createElement('div');
          d.className = 'mutation-desc';
          d.textContent = m.description;
          text.append(d);
        }
        const btn = document.createElement('button');
        btn.className = 'btn';
        btn.type = 'button';
        btn.textContent = 'Run';
        btn.setAttribute('aria-label', `Run mutation: ${m.title}`);
        btn.addEventListener('click', () => this.dispatchEvent(new CustomEvent('mutate', { bubbles: true, composed: true, detail: m.id })));
        row.append(text, btn);
        return row;
      }),
    );
  }
  setState({ streaming, simPaused, connected, finished }) {
    this.refs.stream.textContent = streaming ? 'Pause stream' : 'Resume stream';
    this.refs.sim.textContent = simPaused ? 'Resume system' : 'Pause system';
    this.refs.sim.disabled = !connected || finished;
    for (const b of this.refs.mutations.querySelectorAll('button')) b.disabled = !connected || finished;
  }
}

class TbSystems extends Base {
  constructor() {
    super();
    this.rows = new Map();
  }
  update(systems) {
    const seen = new Set();
    for (const s of systems) {
      seen.add(s.name);
      let row = this.rows.get(s.name);
      if (!row) {
        row = document.getElementById('tb-system-row').content.firstElementChild.cloneNode(true);
        $(row, '.system-name').textContent = s.name;
        this.rows.set(s.name, row);
        this.refs.list.append(row);
      }
      $(row, '.system-rate').textContent = `${abbreviate(s.delivered)} events/s`;
      const set = (k, v) => ($(row, `[data-k="${k}"]`).textContent = v);
      set('actors', abbreviate(s.actors));
      set('runnable', abbreviate(s.runnable));
      set('region', bytes(s.regionBytes));
      set('heap', bytes(s.jsHeap));
      $(row, '[data-k="m-actors"]').set(s.actors, s.actorsCap);
      $(row, '[data-k="m-events"]').set(s.events, s.eventsCap);
      $(row, '[data-k="m-timers"]').set(s.timers, s.timersCap);
      $(row, '[data-k="m-ws"]').set(s.workspaces, s.workspacesCap);
    }
    for (const [name, row] of this.rows)
      if (!seen.has(name)) {
        row.remove();
        this.rows.delete(name);
      }
  }
}

class TbLog extends Base {
  clear() {
    this.refs.list.replaceChildren();
  }
  add(entries) {
    const list = this.refs.list;
    for (const e of entries) {
      const li = document.createElement('li');
      const time = document.createElement('time');
      time.textContent = clock(e.t);
      const lvl = document.createElement('span');
      lvl.className = `lvl ${e.level}`;
      lvl.title = e.level;
      const msg = document.createElement('span');
      msg.className = 'msg';
      const src = document.createElement('span');
      src.className = 'src';
      src.textContent = `${e.source} `;
      msg.append(src, document.createTextNode(e.message));
      li.append(time, lvl, msg);
      list.prepend(li);
    }
    while (list.children.length > 200) list.lastElementChild.remove();
  }
}

/** A table of bench.result() rows (columns = union of keys, minus the time). */
function resultsTable(rows) {
  const cols = [...new Set(rows.flatMap((r) => Object.keys(r)))].filter((k) => k !== 't');
  // camelCase -> sentence case: probeP99Ms -> "Probe p99 (ms)", jsBytesPerActor -> "JS bytes per actor"
  const label = (k) => {
    const words = k.replace(/([a-z0-9])([A-Z])/g, '$1 $2').split(' ').map((w) => w.toLowerCase());
    let unit = '';
    if (words.length > 1 && ['ms', 'mb', 'kb'].includes(words.at(-1))) unit = ` (${words.pop()})`;
    const text = words.map((w) => (w === 'js' ? 'JS' : w)).join(' ') + unit;
    return text.charAt(0).toUpperCase() + text.slice(1);
  };
  const table = document.createElement('table');
  table.className = 'results';
  const head = table.createTHead().insertRow();
  for (const c of cols) {
    const th = document.createElement('th');
    th.textContent = label(c);
    th.scope = 'col';
    // numeric columns are right-aligned, header included; text columns (a reason) left
    if (rows.every((r) => r[c] == null || typeof r[c] === 'number')) th.className = 'n';
    head.append(th);
  }
  const body = table.createTBody();
  for (const r of rows) {
    const tr = body.insertRow();
    for (const c of cols) {
      const td = tr.insertCell();
      const v = r[c];
      td.textContent = v == null ? '–' : typeof v === 'number' ? (/bytes/i.test(c) ? bytes(v) : v >= 1000 ? Math.round(v).toLocaleString('en-US') : String(v)) : String(v);
      if (typeof v === 'number') td.className = 'n';
    }
  }
  const scroll = document.createElement('div');
  scroll.className = 'results-scroll';
  scroll.append(table);
  return scroll;
}

/* --------------------------------------------------------------- the app */

const CHARTS = [
  { id: 'delivered', title: 'Events delivered', unit: 'events/s', value: (s) => s.delivered },
  { id: 'actors', title: 'Live actors', unit: 'actors', value: (s) => s.actors },
  { id: 'remote', title: 'Cross-system sends', unit: 'sends/s', value: (s) => s.remoteOut },
  { id: 'busy', title: 'Thread time', unit: '% of wall clock', format: 'pct', max: 100, value: (s) => s.busy * 100 },
  { id: 'turns', title: 'Scheduling turns', unit: 'turns/s', value: (s) => s.turns },
  { id: 'events', title: 'Event pool in use', unit: 'events', value: (s) => s.events },
  { id: 'timers', title: 'Pending timers', unit: 'timers', value: (s) => s.timers },
  { id: 'region', title: 'System memory', unit: 'region bytes', format: 'bytes', value: (s) => s.regionBytes },
  { id: 'heap', title: 'ECMAScript heap', unit: 'bytes', format: 'bytes', value: (s) => s.jsHeap },
];

const KPIS = [
  { id: 'rate', label: 'Events delivered', unit: '/s', value: (x) => x.totals.delivered },
  { id: 'actors', label: 'Live actors', value: (x) => x.totals.actors },
  { id: 'remote', label: 'Cross-system', unit: '/s', value: (x) => x.totals.remote },
  { id: 'busy', label: 'Thread time', format: 'pct', value: (x) => Math.min(100, x.totals.busy * 100) },
  { id: 'region', label: 'System memory', format: 'bytes', value: (x) => x.totals.regionBytes },
  { id: 'faults', label: 'Faults', value: (x) => x.totals.faults },
];

class TbApp extends Base {
  constructor() {
    super();
    this.samples = [];
    this.window = 120;
    this.streaming = true;
    this.simPaused = false;
    this.connected = false;
    this.finished = false;
    this.charts = new Map();
    this.kpis = new Map();
    this.metricCharts = new Map();
    this.render = scheduler(() => this.draw());
    for (const k of KPIS) {
      const el = document.createElement('tb-stat');
      this.refs.kpis.append(el);
      el.configure(k);
      this.kpis.set(k.id, el);
    }
    for (const c of CHARTS) this.addChart(c.id, c);
    this.addEventListener('stream', () => {
      this.streaming = !this.streaming;
      if (this.streaming) this.render();
      this.syncControls();
    });
    this.addEventListener('sim', () => {
      this.simPaused = !this.simPaused;
      this.send({ cmd: this.simPaused ? 'pause' : 'resume' });
      this.syncControls();
    });
    this.addEventListener('window', (e) => {
      this.window = e.detail;
      this.render();
    });
    this.addEventListener('mutate', (e) => this.send({ cmd: 'mutate', id: e.detail }));
    // the scenario this page was opened for (chamber.html?scenario=<id>)
    this.wanted = new URLSearchParams(location.search).get('scenario');
    this.awaiting = this.wanted;
    this.connect(0);
    setInterval(() => this.tick(), 250);
  }

  addChart(id, def) {
    const el = document.createElement('tb-chart');
    this.refs.charts.append(el);
    el.configure(def);
    this.charts.set(id, { el, def });
    return el;
  }

  send(msg) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(msg));
  }

  connect(attempt) {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(`${proto}//${location.host}/ws`);
    this.ws = ws;
    this.refs.conn.set(attempt ? 'Reconnecting' : 'Connecting', 'warn');
    ws.addEventListener('open', () => {
      this.connected = true;
      this.refs.conn.set('Live', 'ok');
      if (this.awaiting) this.send({ cmd: 'open', scenario: this.awaiting });
      this.syncControls();
    });
    ws.addEventListener('message', (e) => this.onMessage(JSON.parse(e.data)));
    ws.addEventListener('close', () => {
      this.connected = false;
      this.refs.conn.set('Disconnected', 'danger');
      this.syncControls();
      setTimeout(() => this.connect(attempt + 1), Math.min(5000, 500 * 2 ** attempt));
    });
  }

  onMessage(m) {
    switch (m.type) {
      case 'hello': {
        // waiting for our scenario to start: ignore the one still running
        if (this.awaiting && m.id !== this.awaiting) break;
        this.awaiting = null;
        // someone switched the bench to another scenario: follow it
        if (this.shown && m.id && m.id !== this.shown) {
          location.replace(`chamber.html?scenario=${encodeURIComponent(m.id)}`);
          break;
        }
        if (m.id && m.id !== this.wanted) history.replaceState(null, '', `chamber.html?scenario=${encodeURIComponent(m.id)}`);
        this.shown = m.id;
        this.refs.idle.hidden = m.state !== 'idle';
        this.refs.layout.hidden = m.state === 'idle'; // no empty charts under "No scenario is running"
        if (m.state === 'idle') {
          this.refs.kpis.hidden = true;
          this.refs.title.textContent = 'No scenario';
          this.refs.run.set('Idle', 'neutral');
          break;
        }
        const s = m.scenario || {};
        this.refs.title.textContent = s.title || s.name || 'Scenario';
        document.title = `${s.title || 'Scenario'} · Torture chamber`;
        this.refs.description.textContent = s.description || '';
        this.duration = m.duration;
        this.refs.controls.setMutations(s.mutations || []);
        this.metricInfo = s.metrics || {};
        this.refs.kpis.hidden = false;
        this.samples = [];
        this.refs.log.clear(); // the history that follows has this run's log
        this.finished = false;
        this.refs.banner.hidden = true;
        this.finished = m.state === 'finished';
        this.refs.run.set(this.finished ? 'Finished' : 'Running', this.finished ? 'neutral' : 'accent');
        this.syncControls();
        break;
      }
      case 'history':
        if (this.awaiting) break;
        for (const s of m.samples) this.addSample(s);
        this.refs.log.add(m.logs || []);
        this.render();
        break;
      case 'sample':
        if (this.awaiting) break;
        this.addSample(m);
        if (this.streaming) this.render();
        break;
      case 'log':
        if (this.awaiting) break; // the run we wait for: its history brings these
        this.refs.log.add([m]);
        break;
      case 'summary':
        if (this.awaiting) break;
        this.finish(m);
        break;
    }
  }

  addSample(s) {
    this.samples.push(s);
    if (this.samples.length > 2400) this.samples.shift();
    this.simPaused = !!s.paused;
  }

  finish(summary) {
    this.finished = true;
    this.refs.run.set(summary.ok ? 'Finished · passed' : 'Finished · failed', summary.ok ? 'ok' : 'danger');
    const b = this.refs.banner;
    b.hidden = false;
    b.className = `banner ${summary.ok ? 'ok' : 'fail'}`;
    b.replaceChildren();
    const title = document.createElement('div');
    title.className = 'banner-title';
    const n = summary.expect.length;
    const rate = summary.totals.deliveredPerSecond;
    title.textContent = summary.ok
      ? `Passed ${n} ${n === 1 ? 'check' : 'checks'}${rate > 0 ? ` · ${abbreviate(rate)} events/s on average` : ''}`
      : `${summary.failures.length} of ${n} checks failed`;
    b.append(title);
    const ul = document.createElement('ul');
    for (const f of summary.ok ? summary.expect : summary.failures) {
      const li = document.createElement('li');
      li.textContent = f;
      ul.append(li);
    }
    b.append(ul);
    if (summary.results && summary.results.length) b.append(resultsTable(summary.results));
    const actions = document.createElement('div');
    actions.className = 'banner-actions';
    const again = document.createElement('button');
    again.className = 'btn primary';
    again.type = 'button';
    again.textContent = 'Run again';
    again.addEventListener('click', () => {
      this.awaiting = this.shown;
      this.send({ cmd: 'restart' });
    });
    const home = document.createElement('a');
    home.className = 'btn';
    home.href = '/';
    home.textContent = 'All scenarios';
    home.style.cssText = 'display:inline-flex;align-items:center;text-decoration:none';
    actions.append(again, home);
    b.append(actions);
    this.syncControls();
  }

  syncControls() {
    this.refs.controls.setState(this);
  }

  tick() {
    const last = this.samples[this.samples.length - 1];
    if (!last) return;
    this.refs.clock.textContent = this.duration ? `${clock(last.t)} / ${clock(this.duration)}` : clock(last.t);
  }

  draw() {
    const samples = this.samples;
    const last = samples[samples.length - 1];
    if (!last) return;
    const now = last.t;
    const from = now - this.window - 1;
    const recent = samples.filter((s) => s.t >= from);
    const names = [...new Set(recent.flatMap((s) => s.systems.map((x) => x.name)))];
    for (const { el, def } of this.charts.values()) {
      if (def.metric) continue;
      const series = names.map((name) => ({
        name,
        points: recent.flatMap((s) => {
          const x = s.systems.find((y) => y.name === name);
          return x ? [[s.t, def.value(x)]] : [];
        }),
      }));
      el.update(series, now, this.window);
    }
    // scenario metrics: one chart per prefix (e.g. "probeLatencyMs.p50/p99")
    const groups = new Map();
    for (const key of Object.keys(last.metrics || {})) {
      const [prefix, ...rest] = key.split('.');
      if (!groups.has(prefix)) groups.set(prefix, []);
      groups.get(prefix).push({ key, name: rest.join('.') || prefix });
    }
    for (const [prefix, keys] of groups) {
      let entry = this.charts.get(`metric:${prefix}`);
      if (!entry) {
        const latency = /ms$/i.test(prefix);
        const info = (this.metricInfo || {})[prefix] || {};
        const def = {
          metric: true,
          // sentence case, the unit says "milliseconds": probeLatencyMs -> "Probe latency"
          title: info.title || prefix.replace(/(.)Ms$/, '$1').replace(/([a-z0-9])([A-Z])/g, (_, a, b) => `${a} ${b.toLowerCase()}`).replace(/^./, (c) => c.toUpperCase()),
          unit: info.unit || (latency ? 'milliseconds' : 'scenario metric'),
          format: info.format || (latency ? 'ms' : 'num'),
        };
        this.addChart(`metric:${prefix}`, def);
        entry = this.charts.get(`metric:${prefix}`);
      }
      entry.el.update(
        keys.map(({ key, name }) => ({ name, points: recent.filter((s) => s.metrics && key in s.metrics).map((s) => [s.t, s.metrics[key]]) })),
        now,
        this.window,
      );
    }
    for (const k of KPIS) {
      const el = this.kpis.get(k.id);
      el.set(k.value(last), recent.slice(-120).map((s) => k.value(s)));
    }
    this.refs.systems.update(last.systems);
  }
}

customElements.define('tb-status', TbStatus);
customElements.define('tb-meter', TbMeter);
customElements.define('tb-stat', TbStat);
customElements.define('tb-chart', TbChart);
customElements.define('tb-controls', TbControls);
customElements.define('tb-systems', TbSystems);
customElements.define('tb-log', TbLog);
customElements.define('tb-app', TbApp);
