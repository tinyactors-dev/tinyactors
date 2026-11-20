// The anthill's live costs: small multiples, one measure each over the last
// minute (one point a second), so no two scales share an axis. A chart is
// one line in the design system's accent, with its title and current value
// in text ink, a baseline at zero and a labelled top line; hovering shows a
// crosshair and the value at that second. A chart may draw a limit, a
// dashed line with its value, which its scale always shows.

const POINTS = 60;
const WIDTH = 240, HEIGHT = 64, TOP = 4;

export function chart({ title, format, describe = format }) {
  const values = [];
  const current = el('span', { class: 'cost__value' });
  const svg = svgEl('svg', { class: 'cost__plot', viewBox: `0 0 ${WIDTH} ${HEIGHT}`, preserveAspectRatio: 'none', role: 'img' });
  const top = svgEl('line', { class: 'cost__grid', x1: 0, x2: WIDTH, y1: TOP, y2: TOP });
  const base = svgEl('line', { class: 'cost__axis', x1: 0, x2: WIDTH, y1: HEIGHT - 1, y2: HEIGHT - 1 });
  const line = svgEl('polyline', { class: 'cost__line', fill: 'none' });
  const cross = svgEl('line', { class: 'cost__cross', y1: 0, y2: HEIGHT, visibility: 'hidden' });
  const dot = svgEl('circle', { class: 'cost__dot', r: 4, visibility: 'hidden' });
  const limitLine = svgEl('line', { class: 'cost__limit', x1: 0, x2: WIDTH, visibility: 'hidden' });
  svg.append(top, base, limitLine, line, cross, dot);
  const scale = el('span', { class: 'cost__scale' });
  const limitLabel = el('span', { class: 'cost__limit-label', hidden: '' });
  const tip = el('span', { class: 'cost__tip', hidden: '' });
  const element = el('figure', { class: 'cost' },
    el('figcaption', { class: 'cost__head' }, el('span', { class: 'cost__title' }, title), current),
    el('div', { class: 'cost__frame' }, svg, scale, limitLabel, tip));

  let max = 1, limit = null;
  const x = (i) => (values.length < 2 ? WIDTH : (i / (POINTS - 1)) * WIDTH + (POINTS - values.length) * (WIDTH / (POINTS - 1)));
  const y = (v) => HEIGHT - 1 - (v / max) * (HEIGHT - 1 - TOP);

  function render() {
    max = nice(Math.max(...values, limit ?? 0, 0) || 1);
    if (limit !== null) {
      for (const k of ['y1', 'y2']) limitLine.setAttribute(k, y(limit).toFixed(1));
      limitLine.setAttribute('visibility', 'visible');
      limitLabel.textContent = `limit ${format(limit)}`;
      limitLabel.style.top = `${(y(limit) / HEIGHT) * 100}%`;
      limitLabel.hidden = false;
    }
    line.setAttribute('points', values.map((v, i) => `${x(i).toFixed(1)},${y(v).toFixed(1)}`).join(' '));
    scale.textContent = format(max);
    const last = values.at(-1);
    current.textContent = last === undefined ? '–' : format(last);
    svg.setAttribute('aria-label', `${title}: ${last === undefined ? 'no data yet' : describe(last)} now`);
  }

  // Hover: the nearest second, its value and how long ago.
  svg.addEventListener('pointermove', (event) => {
    if (values.length === 0) return;
    const box = svg.getBoundingClientRect();
    const px = ((event.clientX - box.left) / box.width) * WIDTH;
    let best = 0;
    values.forEach((_, i) => { if (Math.abs(x(i) - px) < Math.abs(x(best) - px)) best = i; });
    const cx = x(best), cy = y(values[best]);
    for (const [k, v] of [['x1', cx], ['x2', cx]]) cross.setAttribute(k, v);
    dot.setAttribute('cx', cx); dot.setAttribute('cy', cy);
    cross.setAttribute('visibility', 'visible'); dot.setAttribute('visibility', 'visible');
    const ago = values.length - 1 - best;
    tip.textContent = `${format(values[best])} · ${ago === 0 ? 'now' : `${ago} s ago`}`;
    tip.style.left = `${(cx / WIDTH) * 100}%`;
    tip.hidden = false;
  });
  svg.addEventListener('pointerleave', () => {
    cross.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden');
    tip.hidden = true;
  });

  render();
  return {
    element,
    push(value) {
      values.push(value);
      if (values.length > POINTS) values.shift();
      render();
    },
    clear() { values.length = 0; render(); },
    /** Draws `value` as the chart's limit. */
    setLimit(value) { limit = value; render(); },
  };
}

/** The smallest of 1, 2, 2.5 or 5 times a power of ten at or above `v`. */
function nice(v) {
  const power = 10 ** Math.floor(Math.log10(v));
  return [1, 2, 2.5, 5, 10].map((m) => m * power).find((step) => step >= v);
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}

function svgEl(tag, attributes = {}) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  return node;
}
