// <ta-trace-view>: a trace as a span tree on a timeline, Jaeger's shape.
//
//   const view = document.querySelector('ta-trace-view');
//   view.spans = decodeTraces(bytes); // the spans of a host's telemetry
//
// Rows follow each span's parent, sorted by start: a macrostep holds its
// microsteps, and a step caused by a send sits under the step that sent
// it. Several roots share one trace (spawned, bootstrap, finished). A span
// whose parent is not among the spans hangs under one "missing parent"
// row per parent. A row toggles its children; its name opens the span's
// details: ids, parent, attributes and events.
//
// Colours come from the design system's data tokens, one per chart; an
// error span is marked with the error token. It writes into its own light
// DOM, so the page's stylesheet styles it (style/site.css, .trace-*).

const ATTRIBUTE = {
  chart: 'tinyactors.chart.name',
  actor: 'tinyactors.actor.name',
  entered: 'scxml.state.entered',
  state: 'scxml.state',
  final: 'scxml.final_state',
};
const STATUS_ERROR = 2;

export class TraceView extends HTMLElement {
  #spans = [];
  #collapsed = new Set();
  #selected = null;

  set spans(spans) {
    this.#spans = [...spans].map((span) => ({ ...span, start: Number(span.start), end: Number(span.end) }));
    this.#collapsed.clear();
    this.#selected = null;
    this.render();
  }

  get spans() {
    return this.#spans;
  }

  connectedCallback() {
    this.render();
  }

  render() {
    this.replaceChildren();
    this.classList.add('trace');
    if (this.#spans.length === 0) return;

    const roots = buildTree(this.#spans);
    const t0 = Math.min(...this.#spans.map((span) => span.start));
    const total = Math.max(Math.max(...this.#spans.map((span) => span.end)) - t0, 1);
    const axis = ticks(total);
    const pct = (ns) => `${Math.min(100, Math.max(0, (ns / total) * 100))}%`;
    const colors = chartColors(this.#spans);
    const byID = new Map(this.#spans.map((span) => [span.span, span]));

    const table = el('div', { class: 'trace__table', role: 'tree', 'aria-label': 'Spans' });
    const head = el('div', { class: 'trace__row trace__row--axis' },
      el('div', { class: 'trace__name' }, 'Span'),
      el('div', { class: 'trace__bars' }, ...axis.map((t) =>
        el('span', { class: 'trace__tick', style: `left: ${pct(t)}` }, formatTime(t)))));
    table.append(head);

    for (const { node, depth } of flatten(roots, this.#collapsed)) {
      const span = node.span;
      const open = !this.#collapsed.has(node.key);
      const error = span?.status === STATUS_ERROR;
      const row = el('div', {
        class: `trace__row${span && span.span === this.#selected ? ' trace__row--selected' : ''}${error ? ' trace__row--error' : ''}`,
        role: 'treeitem',
        'aria-level': String(depth + 1),
        ...(node.children.length ? { 'aria-expanded': String(open) } : {}),
        'data-span': span?.span ?? node.key,
      });
      const name = el('div', { class: 'trace__name', style: `padding-left: ${8 + depth * 16}px` });
      if (node.children.length) {
        const toggle = el('button', { class: 'trace__toggle', type: 'button', 'aria-label': open ? 'Collapse' : 'Expand' }, open ? '▾' : '▸');
        toggle.addEventListener('click', () => {
          if (open) this.#collapsed.add(node.key);
          else this.#collapsed.delete(node.key);
          this.render();
        });
        name.append(toggle);
      } else {
        name.append(el('span', { class: 'trace__toggle', 'aria-hidden': 'true' }));
      }
      if (span) {
        const label = el('button', { class: 'trace__label', type: 'button', 'aria-expanded': String(span.span === this.#selected) },
          el('span', { class: 'trace__swatch', style: `background: ${error ? 'var(--state-error)' : colors.get(span.attributes[ATTRIBUTE.chart])}` }),
          el('span', { class: 'trace__span-name' }, shortName(span.name)),
          ...(typeof span.attributes[ATTRIBUTE.actor] === 'string' ? [el('span', { class: 'trace__actor' }, span.attributes[ATTRIBUTE.actor])] : []),
          ...(hint(span) ? [el('span', { class: 'trace__hint' }, hint(span))] : []),
          ...(!open && node.size ? [el('span', { class: 'trace__count' }, `+${node.size}`)] : []));
        label.addEventListener('click', () => {
          this.#selected = this.#selected === span.span ? null : span.span;
          this.render();
        });
        name.append(label);
      } else {
        name.append(el('span', { class: 'trace__missing', title: `Span ${node.missing} is not among these spans` },
          `missing parent ${node.missing.slice(0, 8)}…`));
      }
      const bars = el('div', { class: 'trace__bars' }, ...axis.map((t) => el('span', { class: 'trace__grid', style: `left: ${pct(t)}` })));
      if (span) {
        const offset = span.start - t0;
        bars.append(
          el('span', { class: 'trace__bar', style: `left: ${pct(offset)}; width: ${pct(span.end - span.start)}; background: ${error ? 'var(--state-error)' : colors.get(span.attributes[ATTRIBUTE.chart])}` }),
          el('span', { class: 'trace__at', style: offset / total > 0.8 ? `right: calc(${100 - (offset / total) * 100}% + 6px)` : `left: calc(${pct(offset)} + 6px)` }, `+${formatTime(offset)}`));
      }
      row.append(name, bars);
      table.append(row);
      if (span && span.span === this.#selected) table.append(this.#detail(span, t0, byID));
    }
    this.append(el('div', { class: 'trace__scroll' }, table));
  }

  #detail(span, t0, byID) {
    const detail = el('div', { class: 'trace__detail' });
    detail.append(el('p', { class: 'trace__detail-head' },
      el('strong', {}, span.name), ` at +${formatTime(span.start - t0)}`, span.statusMessage ? ` · ${span.statusMessage}` : ''));
    const ids = el('dl', { class: 'trace__ids' },
      el('dt', {}, 'Trace'), el('dd', {}, span.trace),
      el('dt', {}, 'Span'), el('dd', {}, span.span),
      el('dt', {}, 'Parent'));
    if (!span.parent) {
      ids.append(el('dd', {}, 'none (a root)'));
    } else if (byID.has(span.parent)) {
      const parent = el('button', { class: 'trace__link', type: 'button' }, `${span.parent} (${shortName(byID.get(span.parent).name)})`);
      parent.addEventListener('click', () => this.reveal(span.parent));
      ids.append(el('dd', {}, parent));
    } else {
      ids.append(el('dd', {}, `${span.parent} (not among these spans)`));
    }
    detail.append(ids);
    const attributes = Object.entries(span.attributes).sort(([a], [b]) => a.localeCompare(b));
    detail.append(el('table', { class: 'trace__attributes' },
      el('caption', {}, `Attributes (${attributes.length})`),
      el('tbody', {}, ...attributes.map(([key, value]) => el('tr', {}, el('th', { scope: 'row' }, key), el('td', {}, display(value)))))));
    if (span.events.length) {
      detail.append(el('ul', { class: 'trace__events', 'aria-label': `Events (${span.events.length})` },
        ...span.events.map((event) => el('li', {},
          el('strong', {}, shortName(event.name)), ' ',
          Object.entries(event.attributes).map(([key, value]) => `${shortName(key)}=${display(value)}`).join(' ')))));
    }
    return detail;
  }

  /** Opens every ancestor of a span, selects it and scrolls to it. */
  reveal(id) {
    const byID = new Map(this.#spans.map((span) => [span.span, span]));
    for (let parent = byID.get(id)?.parent; parent; parent = byID.get(parent)?.parent) {
      this.#collapsed.delete(parent);
      this.#collapsed.delete(`missing:${parent}`);
    }
    this.#selected = id;
    this.render();
    this.querySelector(`[data-span="${id}"]`)?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }
}

// ---- the tree -------------------------------------------------------------

export function buildTree(spans) {
  const nodes = new Map(spans.map((span) => [span.span, { key: span.span, span, missing: null, children: [], start: span.start, end: span.end, size: 0 }]));
  const roots = [];
  const missing = new Map();
  for (const span of spans) {
    const node = nodes.get(span.span);
    if (!span.parent) {
      roots.push(node);
    } else if (nodes.has(span.parent) && span.parent !== span.span) {
      nodes.get(span.parent).children.push(node);
    } else {
      let group = missing.get(span.parent);
      if (!group) {
        group = { key: `missing:${span.parent}`, span: null, missing: span.parent, children: [], start: span.start, end: span.end, size: 0 };
        missing.set(span.parent, group);
        roots.push(group);
      }
      group.children.push(node);
      group.start = Math.min(group.start, span.start);
      group.end = Math.max(group.end, span.end);
    }
  }
  const byStart = (a, b) => a.start - b.start || a.end - b.end;
  const finish = (node) => {
    node.children.sort(byStart);
    node.size = node.children.reduce((sum, child) => sum + 1 + finish(child), 0);
    return node.size;
  };
  roots.sort(byStart);
  roots.forEach(finish);
  return roots;
}

/** Depth-first rows, without the children of collapsed nodes. */
export function flatten(roots, collapsed) {
  const rows = [];
  const walk = (node, depth) => {
    rows.push({ node, depth });
    if (!collapsed.has(node.key)) node.children.forEach((child) => walk(child, depth + 1));
  };
  roots.forEach((root) => walk(root, 0));
  return rows;
}

// ---- formatting -----------------------------------------------------------

/** Nanoseconds as "0", "850 ns", "12 µs", "3.4 ms", "90 s", "15 min":
 * seconds up to ten minutes, so an axis reads 25 s, 50 s, 75 s. */
export function formatTime(ns) {
  if (!Number.isFinite(ns)) return '–';
  if (ns === 0) return '0';
  // [from, unit, nanoseconds per unit]
  const units = [[600e9, 'min', 60e9], [1e9, 's', 1e9], [1e6, 'ms', 1e6], [1e3, 'µs', 1e3], [0, 'ns', 1]];
  const [, unit, scale] = units.find(([from]) => Math.abs(ns) >= from);
  const value = ns / scale;
  return `${Number.isInteger(value) ? value : value.toFixed(value < 10 ? 2 : 1)} ${unit}`;
}

/** Evenly spaced round offsets across [0, total]. */
export function ticks(total, count = 5) {
  const raw = total / count;
  const pow = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * pow).find((s) => s >= raw) ?? raw;
  const out = [];
  for (let t = 0; t <= total + step * 1e-9; t += step) out.push(t);
  return out;
}

const shortName = (name) => name.replace(/^scxml\./, '');

/** After the name: the states a step entered, or the final state. */
function hint(span) {
  const final = span.attributes[ATTRIBUTE.final];
  if (typeof final === 'string') return `final: ${final}`;
  const entered = span.attributes[ATTRIBUTE.entered] ?? (span.name.startsWith('scxml.macrostep') ? span.attributes[ATTRIBUTE.state] : null);
  return Array.isArray(entered) && entered.length ? `→ ${entered.join(', ')}` : null;
}

function display(value) {
  if (typeof value === 'string') return value;
  return JSON.stringify(value, (key, v) => (typeof v === 'bigint' ? Number(v) : v));
}

/** One data token per chart, in the order they appear; they repeat after six. */
function chartColors(spans) {
  const colors = new Map();
  for (const span of spans) {
    const chart = span.attributes[ATTRIBUTE.chart];
    if (!colors.has(chart)) colors.set(chart, chart === undefined ? 'var(--fg-4)' : `var(--data-${(colors.size % 6) + 1})`);
  }
  return colors;
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children.filter((child) => child !== null && child !== undefined && child !== false));
  return node;
}

if (!customElements.get('ta-trace-view')) customElements.define('ta-trace-view', TraceView);
