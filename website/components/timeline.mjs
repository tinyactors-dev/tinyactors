// A run's timeline: panels of measures that share one time axis, one point
// a second over the whole run (the last 15 minutes of a longer one). Each
// panel has one y-scale from zero, its own measures in it (never a second
// axis). Its header is its legend: a line key, the measure's name and its
// value, the latest or the hovered second's. A crosshair runs through every
// panel; the arrow keys move it when a panel has focus. A panel may draw a
// reference, a dashed line in muted ink (the lab's target population).
// A second series is dashed as well as coloured, so it never rests on
// colour alone. Under the panels, a table of the run, every fifth second.

const W = 600, H = 120, TOP = 6;
const KEEP = 900; // seconds kept
const svgNS = 'http://www.w3.org/2000/svg';

/**
 * panels: [{ id, title, format, series: [{ key, label }], reference?: { key, label } }]
 * The first series of a panel is drawn in the accent, a second dashed.
 */
export function timeline({ panels }) {
  const times = [];
  const columns = new Map(); // key -> values, aligned with times
  let hover = null; // index under the crosshair, or null for the latest
  const at = el('span', { class: 'timeline__at', 'aria-live': 'off' });
  const views = panels.map((panel) => panelView(panel));
  const axis = el('div', { class: 'timeline__axis', 'aria-hidden': 'true' });
  const table = el('details', { class: 'timeline__table' }, el('summary', {}, 'Table of the run'));
  table.addEventListener('toggle', () => { if (table.open) fillTable(); });
  const element = el('div', { class: 'timeline' }, el('div', { class: 'timeline__head' }, at), ...views.map((v) => v.element), axis, table);

  const x = (i) => (times.length < 2 ? W : ((times[i] - times[0]) / Math.max(1, times.at(-1) - times[0])) * W);

  function panelView(panel) {
    const keys = [...panel.series.map((s) => s.key), ...(panel.reference ? [panel.reference.key] : [])];
    for (const key of keys) columns.set(key, []);
    const svg = svgEl('svg', { class: 'timeline__plot', viewBox: `0 0 ${W} ${H}`, preserveAspectRatio: 'none', tabindex: '0', role: 'img' });
    const grid = svgEl('line', { class: 'timeline__grid', x1: 0, x2: W, y1: TOP, y2: TOP });
    const base = svgEl('line', { class: 'timeline__base', x1: 0, x2: W, y1: H - 1, y2: H - 1 });
    const reference = panel.reference ? svgEl('polyline', { class: 'timeline__reference', fill: 'none' }) : null;
    const lines = panel.series.map((_, i) => svgEl('polyline', { class: `timeline__line timeline__line--${i === 0 ? 'first' : 'second'}`, fill: 'none' }));
    const cross = svgEl('line', { class: 'timeline__cross', y1: 0, y2: H, visibility: 'hidden' });
    svg.append(grid, base, ...(reference ? [reference] : []), ...lines, cross);
    const scale = el('span', { class: 'timeline__scale' });
    const values = new Map();
    const legend = el('div', { class: 'timeline__legend' },
      el('span', { class: 'timeline__title' }, panel.title),
      ...[...panel.series.map((s, i) => [s, i === 0 ? 'first' : 'second']), ...(panel.reference ? [[panel.reference, 'reference']] : [])]
        .map(([s, kind]) => {
          const value = el('strong', { class: 'timeline__value' });
          values.set(s.key, value);
          return el('span', { class: 'timeline__item' }, el('span', { class: `timeline__key timeline__key--${kind}`, 'aria-hidden': 'true' }), s.label === panel.title ? '' : `${s.label} `, value);
        }));
    const element = el('figure', { class: 'timeline__panel' }, legend, el('div', { class: 'timeline__frame' }, svg, scale));
    let max = 1;
    const y = (v) => H - 1 - (v / max) * (H - 1 - TOP);
    const points = (key) => columns.get(key).map((v, i) => (v === null ? null : `${x(i).toFixed(1)},${y(v).toFixed(1)}`)).filter(Boolean).join(' ');

    function render() {
      max = nice(Math.max(1, ...keys.flatMap((key) => columns.get(key).filter((v) => v !== null))));
      scale.textContent = panel.format(max);
      panel.series.forEach((s, i) => lines[i].setAttribute('points', points(s.key)));
      if (reference) reference.setAttribute('points', points(panel.reference.key));
      readout();
    }
    function readout() {
      const i = hover ?? times.length - 1;
      for (const key of keys) {
        const v = i >= 0 ? columns.get(key)[i] : null;
        values.get(key).textContent = v === null || v === undefined ? '–' : panel.format(v);
      }
      if (hover === null) cross.setAttribute('visibility', 'hidden');
      else {
        cross.setAttribute('x1', x(hover)); cross.setAttribute('x2', x(hover));
        cross.setAttribute('visibility', 'visible');
      }
      const last = times.length ? columns.get(panel.series[0].key).at(-1) : null;
      svg.setAttribute('aria-label', `${panel.title}: ${last === null || last === undefined ? 'no data yet' : panel.format(last)} now; arrow keys move through the run`);
    }

    // The nearest second under the pointer; the arrow keys step through them.
    svg.addEventListener('pointermove', (event) => {
      if (times.length === 0) return;
      const box = svg.getBoundingClientRect();
      const px = ((event.clientX - box.left) / box.width) * W;
      let best = 0;
      for (let i = 0; i < times.length; i++) if (Math.abs(x(i) - px) < Math.abs(x(best) - px)) best = i;
      setHover(best);
    });
    svg.addEventListener('pointerleave', () => setHover(null));
    svg.addEventListener('blur', () => setHover(null));
    svg.addEventListener('keydown', (event) => {
      if (times.length === 0) return;
      const step = { ArrowLeft: -1, ArrowRight: 1, PageUp: -10, PageDown: 10 }[event.key];
      if (event.key === 'Home') setHover(0);
      else if (event.key === 'End') setHover(times.length - 1);
      else if (step) setHover(Math.min(times.length - 1, Math.max(0, (hover ?? times.length - 1) + step)));
      else return;
      event.preventDefault();
    });
    return { element, render, readout };
  }

  function setHover(index) {
    hover = index;
    at.textContent = index === null ? '' : `at ${clock(times[index])}`;
    views.forEach((v) => v.readout());
  }

  function renderAxis() {
    axis.replaceChildren();
    if (times.length < 2) return;
    const span = times.at(-1) - times[0];
    const step = [10, 30, 60, 120, 300, 600].find((s) => span / s <= 6) ?? 900;
    for (let s = Math.ceil(times[0] / step) * step; s <= times.at(-1); s += step) {
      axis.append(el('span', { class: 'timeline__tick', style: `left: ${((s - times[0]) / span) * 100}%` }, clock(s)));
    }
  }

  function fillTable() {
    const keys = panels.flatMap((p) => [...p.series, ...(p.reference ? [p.reference] : [])].map((s) => [s, p]));
    const head = el('tr', {}, el('th', { scope: 'col' }, 'time'), ...keys.map(([s, p]) => el('th', { scope: 'col' }, s.label === p.title ? p.title : `${p.title}: ${s.label}`)));
    const rows = [];
    for (let i = 0; i < times.length; i++) {
      if (times[i] % 5 !== 0 && i !== times.length - 1) continue;
      rows.push(el('tr', {}, el('th', { scope: 'row' }, clock(times[i])),
        ...keys.map(([s, p]) => { const v = columns.get(s.key)[i]; return el('td', {}, v === null ? '–' : p.format(v)); })));
    }
    table.querySelector('.table')?.remove();
    table.append(el('div', { class: 'table' }, el('div', { class: 'table__scroll', tabindex: '0' }, el('table', {}, el('thead', {}, head), el('tbody', {}, ...rows)))));
  }

  return {
    element,
    /** One second: `t` in whole seconds since the run began, `values` by key (a missing key: no value). */
    push(t, values) {
      times.push(t);
      for (const [key, column] of columns) column.push(values[key] ?? null);
      if (times.length > KEEP) { times.shift(); for (const column of columns.values()) column.shift(); if (hover !== null) hover = Math.max(0, hover - 1); }
      views.forEach((v) => v.render());
      renderAxis();
      if (table.open) fillTable();
    },
    clear() { times.length = 0; for (const column of columns.values()) column.length = 0; hover = null; at.textContent = ''; views.forEach((v) => v.render()); renderAxis(); },
    /** Shows or hides the panel `id`. */
    show(id, visible) { views[panels.findIndex((p) => p.id === id)].element.hidden = !visible; },
  };
}

/** The smallest of 1, 2, 2.5 or 5 times a power of ten at or above `v`. */
function nice(v) {
  const power = 10 ** Math.floor(Math.log10(v));
  return [1, 2, 2.5, 5, 10].map((m) => m * power).find((step) => step >= v);
}

const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.round(seconds % 60)).padStart(2, '0')}`;

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}

function svgEl(tag, attributes = {}) {
  const node = document.createElementNS(svgNS, tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  return node;
}
