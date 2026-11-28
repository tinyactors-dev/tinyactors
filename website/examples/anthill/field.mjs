// What the anthill's two views share: the full demo (page.mjs) and the
// home page's field (hero.mjs). launch() starts the two colonies, each in
// a worker.mjs of its own joined by a channel, and keeps their latest
// numbers; dots() draws the ants where WebGL2 is missing; palette() reads
// the design system's colours.
import { WIDTH, HEIGHT, HILLS } from './world.mjs';

/**
 * Starts a field of `population` ants (half in each colony) resting
 * `rest` simulated seconds between outings on average, drawn by
 * `renderer`. Answers the running field: its workers and its latest
 * numbers, which every frame updates before `onFrame(field)`. `onMessage`
 * sees every message a colony sends, after the field took what it needs.
 * stop() ends both workers, and their memory with them: a WebAssembly
 * memory never shrinks. pause(true) holds both colonies where they are,
 * pause(false) lets them go on from there.
 */
export function launch({ population, rest, renderer, onProgress, onReady, onFrame, onError, onMessage }) {
  const workers = HILLS.map(() => new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' }));
  const field = {
    workers, renderer, population, ready: false, stopped: false, stats: null, speed: 1, messages: 0, perMessage: null,
    added: null, sim: 0, wall: 0, memory: null,
    parts: workers.map(() => ({ progress: 0, ready: false, data: null, base: null })),
    stop() { field.stopped = true; workers.forEach((worker) => worker.terminate()); },
    pause(paused) { workers.forEach((worker) => worker.postMessage({ pause: paused })); },
  };
  const channel = new MessageChannel();
  const seed = Math.floor(Math.random() * 2 ** 31);
  workers.forEach((worker, colony) => {
    const part = field.parts[colony];
    worker.onmessage = ({ data }) => {
      if (field.stopped) return;
      if (data.progress !== undefined) {
        part.progress = data.progress;
        onProgress?.(field.parts.reduce((sum, p) => sum + p.progress, 0) / field.parts.length);
      }
      if (data.ready) {
        part.ready = true; part.base = data.base;
        field.ready = field.parts.every((p) => p.ready);
        if (field.ready) onReady?.(field);
      }
      if (data.error) onError?.(data.error.split('\n')[0]);
      if (data.delta) {
        renderer.apply(data.delta);
        if (combine(field, part, data)) onFrame?.(field);
      }
      onMessage?.(data);
    };
    worker.onerror = (event) => onError?.(event.message);
    const port = colony === 0 ? channel.port1 : channel.port2;
    worker.postMessage({ start: true, population: population / 2, colony, seed, offset: colony * (population / 2), rest, port }, [port]);
  });
  return field;
}

// Each colony reports its own frames; the field shows both, at the time of
// the one behind, which the other waits for. Answers whether both have
// reported, so the field's numbers are whole.
function combine(field, part, data) {
  part.data = data;
  const parts = field.parts.map((p) => p.data);
  if (parts.some((p) => !p)) return false;
  const behind = parts.reduce((a, b) => (b.stats.time < a.stats.time ? b : a));
  const stats = {
    time: behind.stats.time,
    colonies: parts.map((p) => p.stats.colony),
    fights: parts.reduce((sum, p) => sum + p.stats.fights, 0),
    food: parts[0].stats.food,
  };
  Object.assign(field, {
    stats, sim: behind.stats.time, wall: behind.wall,
    memory: parts.every((p) => p.memory) ? parts.reduce((sum, p) => sum + p.memory, 0) : null,
    speed: Math.min(...parts.map((p) => p.speed)),
    messages: parts.reduce((sum, p) => sum + p.messages, 0),
    // both colonies' time over both colonies' messages
    perMessage: parts.every((p) => p.perMessage !== null && p.messages > 0)
      ? parts.reduce((sum, p) => sum + p.perMessage * p.messages, 0) / parts.reduce((sum, p) => sum + p.messages, 0) : null,
    // what the ants added to the module's memory: the module and the empty hosts left out
    added: parts.every((p, i) => p.memory && typeof field.parts[i].base === 'number')
      ? parts.reduce((sum, p, i) => sum + p.memory - field.parts[i].base, 0) : null,
  });
  return true;
}

/** The ants of a field: workers and brood of both colonies. */
export const ants = (field) => field.stats.colonies.reduce((sum, c) => sum + c.workers + c.brood, 0);

/** The simulation's time now: the last frame's, moved on by the wall clock at the simulation's speed, never past the next frame. */
export const now = (field) => field.sim + Math.min(0.1, ((performance.now() - field.wall) / 1000) * field.speed);

/** A WebGL canvas draws at the screen's pixels; the 2D fallback at the field's. */
export function fit(canvas, sharp) {
  if (!sharp) return;
  const width = Math.round(canvas.clientWidth * devicePixelRatio), height = Math.round(canvas.clientHeight * devicePixelRatio);
  if (width && (canvas.width !== width || canvas.height !== height)) { canvas.width = width; canvas.height = height; }
}

// Where WebGL2 is missing: a block of pixels per ant, at least one screen
// pixel wide, on a 2D canvas at the field's resolution.
export function dots(canvas, population, colors) {
  const context = canvas.getContext('2d');
  const image = context.createImageData(WIDTH, HEIGHT);
  const pixels = new Uint32Array(image.data.buffer);
  const trips = { x0: new Float32Array(population), y0: new Float32Array(population), x1: new Float32Array(population),
    y1: new Float32Array(population), t0: new Float32Array(population), t1: new Float32Array(population), look: new Uint8Array(population) };
  const ants = [0, ...colors.colonyPixels];
  return {
    kind: '2d',
    apply(delta) {
      for (let i = 0; i < delta.length; i += 8) {
        const slot = delta[i];
        trips.x0[slot] = delta[i + 1]; trips.y0[slot] = delta[i + 2]; trips.x1[slot] = delta[i + 3]; trips.y1[slot] = delta[i + 4];
        trips.t0[slot] = delta[i + 5]; trips.t1[slot] = delta[i + 6]; trips.look[slot] = delta[i + 7];
      }
    },
    draw(t, antSize, view) {
      pixels.fill(colors.groundPixel);
      const base = Math.max(population <= 20_000 ? 2 : 1, Math.ceil(WIDTH / Math.max(1, canvas.clientWidth)));
      const size = Math.min(16, Math.round(Math.max(base, antSize * 0.4 * view.scale)));
      const { left, top, scale } = view;
      for (let slot = 0; slot < population; slot++) {
        const look = trips.look[slot];
        if (!look) continue;
        const span = trips.t1[slot] - trips.t0[slot];
        const f = span > 0 ? Math.min(1, Math.max(0, (t - trips.t0[slot]) / span)) : 1;
        const x = ((trips.x0[slot] + (trips.x1[slot] - trips.x0[slot]) * f - left) * scale) | 0;
        const y = ((trips.y0[slot] + (trips.y1[slot] - trips.y0[slot]) * f - top) * scale) | 0;
        if (x < 0 || y < 0 || x > WIDTH - size || y > HEIGHT - size) continue;
        for (let dy = 0, at = y * WIDTH + x; dy < size; dy++, at += WIDTH) {
          for (let dx = 0; dx < size; dx++) pixels[at + dx] = ants[look];
        }
      }
      context.putImageData(image, 0, 0);
    },
  };
}

/** The design system's colours, as CSS colours and as the 2D canvas's 32-bit pixels. */
export function palette(root) {
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
  const groundHex = css('--surface-1', '#FAF6EE');
  return {
    groundHex,
    groundPixel: pixel(groundHex),
    colony,
    colonyPixels: colony.map(pixel),
    hill: colony,
    food: css('--data-1', '#D8A64E'),
    ink: css('--fg-1', '#2A2622'),
  };
}

export function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
