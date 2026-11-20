// The anthill's page: a canvas, a population slider and two tools. The
// world runs in worker.mjs; this draws each ant on its current leg, at the
// simulation's time, from the legs the worker sends when they change.
import { WIDTH, HEIGHT, HILLS, HILL_RADIUS } from './world.mjs';

const POPULATIONS = [1_000, 10_000, 100_000, 250_000, 500_000, 1_000_000];
const DEFAULT = 2; // 100,000
const number = new Intl.NumberFormat('en');

export function mount(root) {
  const colors = palette(root);
  const canvas = el('canvas', { class: 'anthill__canvas', width: WIDTH, height: HEIGHT, 'aria-label': 'Two anthills; select to drop food or squash ants' });
  const slider = el('input', { type: 'range', min: 0, max: POPULATIONS.length - 1, step: 1, value: DEFAULT, 'aria-label': 'Population' });
  const count = el('output', { class: 'anthill__count' });
  const start = el('button', { type: 'button', class: 'ta-btn ta-btn--primary' }, 'Restart');
  const tools = el('div', { role: 'radiogroup', class: 'ta-tabs ta-tabs--segmented', 'aria-label': 'Tool' });
  let tool = 'food';
  for (const [name, label] of [['food', 'Drop food'], ['squash', 'Squash ants']]) {
    const button = el('button', { type: 'button', role: 'radio', class: 'ta-tab', 'aria-checked': String(name === tool) }, label);
    button.addEventListener('click', () => {
      tool = name;
      for (const other of tools.children) other.setAttribute('aria-checked', String(other === button));
    });
    tools.append(button);
  }
  const status = el('p', { class: 'anthill__status', 'aria-live': 'polite' });
  const hills = el('p', { class: 'anthill__hills' });
  root.append(
    el('div', { class: 'anthill__controls' }, el('label', { class: 'anthill__population' }, 'Ants ', slider, count), start, tools),
    el('div', { class: 'anthill__stage' }, canvas),
    hills, status,
  );
  const showCount = () => { count.textContent = number.format(POPULATIONS[slider.value]); };
  slider.addEventListener('input', showCount);
  showCount();

  const context = canvas.getContext('2d');
  const image = context.createImageData(WIDTH, HEIGHT);
  const pixels = new Uint32Array(image.data.buffer);
  let world = null;

  function begin() {
    world?.worker.terminate(); // its memory goes with it: a WebAssembly memory never shrinks
    const population = POPULATIONS[slider.value];
    const worker = new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' });
    world = {
      worker, population, ready: false,
      x0: new Float32Array(population), y0: new Float32Array(population), x1: new Float32Array(population),
      y1: new Float32Array(population), t0: new Float32Array(population), t1: new Float32Array(population),
      look: new Uint8Array(population), stats: null, speed: 1, rate: 0, sim: 0, wall: 0,
    };
    const current = world;
    status.textContent = `Hatching ${number.format(population)} ants…`;
    worker.onmessage = ({ data }) => {
      if (current !== world) return;
      if (data.progress !== undefined) status.textContent = `Hatching ${number.format(population)} ants… ${Math.round(data.progress * 100)}%`;
      if (data.ready) current.ready = true;
      if (data.error) status.textContent = `The simulation stopped: ${data.error.split('\n')[0]}`;
      if (data.delta) apply(current, data);
    };
    worker.onerror = (event) => { status.textContent = `The simulation stopped: ${event.message}`; };
    worker.postMessage({ start: true, population, seed: Math.floor(Math.random() * 2 ** 31) });
  }
  start.addEventListener('click', begin);

  canvas.addEventListener('pointerdown', (event) => {
    if (!world?.ready) return;
    const box = canvas.getBoundingClientRect();
    const x = ((event.clientX - box.left) / box.width) * WIDTH, y = ((event.clientY - box.top) / box.height) * HEIGHT;
    world.worker.postMessage(tool === 'food' ? { food: true, x, y } : { squash: true, x, y, radius: 16 });
  });

  function apply(state, { delta, stats, speed, rate, wall }) {
    for (let i = 0; i < delta.length; i += 8) {
      const slot = delta[i];
      state.x0[slot] = delta[i + 1]; state.y0[slot] = delta[i + 2]; state.x1[slot] = delta[i + 3]; state.y1[slot] = delta[i + 4];
      state.t0[slot] = delta[i + 5]; state.t1[slot] = delta[i + 6]; state.look[slot] = delta[i + 7];
    }
    Object.assign(state, { stats, speed, rate, sim: stats.time, wall });
    if (!state.ready) return;
    const ants = stats.colonies.reduce((sum, c) => sum + c.workers + c.brood, 0);
    hills.replaceChildren(...stats.colonies.map((c, i) => el('span', { class: `anthill__hill anthill__hill--${i + 1}` },
      `Hill ${i + 1}: ${number.format(c.workers)} workers, ${number.format(c.brood)} brood, ${number.format(c.stored)} food home`)));
    status.textContent = `${number.format(ants + 2)} actors (2 queens) · sim speed ${speed.toFixed(2)}× · ` +
      `${number.format(Math.round(rate))} decisions a second · ${number.format(stats.fights)} fights · ${Math.floor(stats.time)} s`;
  }

  function draw() {
    requestAnimationFrame(draw);
    pixels.fill(colors.ground);
    const state = world;
    if (state?.ready) {
      // The simulation's time now: the last frame's, moved on by the wall
      // clock at the simulation's speed, never past the next frame.
      const t = state.sim + Math.min(0.1, ((performance.now() - state.wall) / 1000) * state.speed);
      // An ant is at least one screen pixel: a canvas shown narrower than
      // the field (a phone) draws it as a block of field pixels.
      const size = Math.min(4, Math.max(state.population <= 20_000 ? 2 : 1, Math.ceil(WIDTH / Math.max(1, canvas.clientWidth))));
      const ants = [0, colors.colony[0], colors.colony[1]];
      for (let slot = 0; slot < state.population; slot++) {
        const look = state.look[slot];
        if (!look) continue;
        const span = state.t1[slot] - state.t0[slot];
        const f = span > 0 ? Math.min(1, Math.max(0, (t - state.t0[slot]) / span)) : 1;
        const x = (state.x0[slot] + (state.x1[slot] - state.x0[slot]) * f) | 0;
        const y = (state.y0[slot] + (state.y1[slot] - state.y0[slot]) * f) | 0;
        if (x < 0 || y < 0 || x > WIDTH - size || y > HEIGHT - size) continue;
        const color = ants[look];
        for (let dy = 0, at = y * WIDTH + x; dy < size; dy++, at += WIDTH) {
          for (let dx = 0; dx < size; dx++) pixels[at + dx] = color;
        }
      }
    }
    context.putImageData(image, 0, 0);
    for (const [x, y, left] of state?.stats?.food ?? []) {
      if (left <= 0) continue;
      context.fillStyle = colors.food;
      context.beginPath(); context.arc(x, y, 3 + Math.min(9, Math.sqrt(left) / 6), 0, Math.PI * 2); context.fill();
    }
    HILLS.forEach((hill, i) => {
      context.fillStyle = colors.hill[i];
      context.strokeStyle = colors.ink;
      context.lineWidth = 1.5;
      context.beginPath(); context.arc(hill.x, hill.y, HILL_RADIUS, 0, Math.PI * 2); context.fill(); context.stroke();
    });
  }
  requestAnimationFrame(draw);

  // Start once the anthill is near the screen.
  const near = new IntersectionObserver(([entry]) => {
    if (!entry.isIntersecting) return;
    near.disconnect();
    begin();
  }, { rootMargin: '200px' });
  near.observe(root);
}

/** The design system's colours, as the canvas's 32-bit pixels. */
function palette(root) {
  const style = getComputedStyle(root);
  const probe = document.createElement('canvas').getContext('2d');
  const css = (name, fallback) => {
    probe.fillStyle = fallback;
    probe.fillStyle = style.getPropertyValue(name).trim() || fallback;
    return probe.fillStyle;
  };
  const pixel = (color) => {
    const [r, g, b] = [1, 3, 5].map((i) => parseInt(color.slice(i, i + 2), 16));
    return (255 << 24) | (b << 16) | (g << 8) | r; // little-endian RGBA
  };
  const colony = [css('--accent', '#1F5E4A'), css('--data-3', '#B8876A')];
  return {
    ground: pixel(css('--surface-1', '#FAF6EE')),
    colony: colony.map(pixel),
    hill: colony,
    food: css('--data-1', '#D8A64E'),
    ink: css('--fg-1', '#2A2622'),
  };
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
