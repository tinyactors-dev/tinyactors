// The anthill's page: the field, a population slider, two tools and the
// live costs. Each colony runs in a worker.mjs of its own; renderer.mjs draws every ant
// with the design system's rigged ant (sprites.mjs) on the GPU, and where
// WebGL2 is missing, this draws a dot per ant on a 2D canvas instead.
// Hills and food are drawn on a 2D canvas above the ants.
import { WIDTH, HEIGHT, HILLS, HILL_RADIUS } from './world.mjs';
import { atlas } from './sprites.mjs';
import { createRenderer } from './renderer.mjs';
import { chart } from './charts.mjs';

const POPULATIONS = [1_000, 10_000, 100_000, 250_000, 500_000, 1_000_000, 2_000_000, 4_000_000];
const DEFAULT = 5; // 1,000,000
// How long a worker rests in the hill between outings, on average, in
// simulated seconds: the longer, the fewer ants out and messages a second.
const RESTS = [5, 10, 30, 60, 120, 300];
const REST = 3; // a minute
const duration = (seconds) => (seconds < 60 ? `${seconds} s` : `${seconds / 60} min`);
const number = new Intl.NumberFormat('en');
const FOOD_RADIUS = 12; // a full source, in field units
const megabytes = (bytes) => `${number.format(Math.round(bytes / 1048576))} MB`;

export async function mount(root) {
  const colors = palette(root);
  const slider = el('input', { type: 'range', min: 0, max: POPULATIONS.length - 1, step: 1, value: DEFAULT, 'aria-label': 'Population' });
  const count = el('output', { class: 'anthill__count' });
  const restSlider = el('input', { type: 'range', min: 0, max: RESTS.length - 1, step: 1, value: REST, 'aria-label': 'Rest between outings' });
  const restCount = el('output', { class: 'anthill__count' });
  const restart = el('button', { type: 'button', class: 'ta-btn ta-btn--primary' }, 'Restart');
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
  const zoomOut = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary', 'aria-label': 'Zoom out' }, '−');
  const zoomIn = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary', 'aria-label': 'Zoom in' }, '+');
  const zoomFit = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary' }, 'Fit');
  const zoomLevel = el('output', { class: 'anthill__zoom-level', 'aria-live': 'polite' });
  const zoom = el('div', { class: 'anthill__zoom', role: 'group', 'aria-label': 'Zoom' }, zoomOut, zoomLevel, zoomIn, zoomFit);
  const overlay = el('canvas', { class: 'anthill__overlay', width: WIDTH, height: HEIGHT, tabindex: '0',
    'aria-label': 'Two anthills. Select to drop food or squash ants; drag to move when zoomed in; plus, minus and 0 zoom; arrow keys move.' });
  const stage = el('div', { class: 'anthill__stage' }, overlay);
  const status = el('p', { class: 'anthill__status', 'aria-live': 'polite' });
  const hills = el('p', { class: 'anthill__hills' });
  const costs = {
    actors: chart({ title: 'Actors', format: (v) => number.format(Math.round(v)) }),
    fps: chart({ title: 'Frames drawn a second', format: (v) => `${Math.round(v)} fps` }),
    // The simulated time that passed in each second of real time: 100% keeps
    // up, 50% means a simulated second took two real ones.
    speed: chart({ title: 'Keeping up with real time', format: (v) => `${Math.round(v * 100)}%`, describe: (v) => `${Math.round(v * 100)} percent of real time` }),
    memory: chart({ title: 'WebAssembly memory', format: megabytes }),
    messages: chart({ title: 'Messages a second', format: (v) => number.format(Math.round(v)) }),
    // A colony's working time over the messages it handled: the host and
    // the world together, the wait for the other colony left out.
    perMessage: chart({ title: 'Time per message', format: (v) => `${v.toFixed(1)} µs`, describe: (v) => `${v.toFixed(1)} microseconds` }),
  };
  root.append(
    el('div', { class: 'anthill__controls' }, el('label', { class: 'anthill__population' }, 'Ants ', slider, count),
      el('label', { class: 'anthill__population' }, 'Rest ', restSlider, restCount), restart, tools, zoom),
    stage, hills, status,
    el('div', { class: 'anthill__costs' }, ...Object.values(costs).map((c) => c.element)),
  );
  let world = null; // the running anthill: its Workers, renderer and latest numbers
  const showCount = () => { count.textContent = number.format(POPULATIONS[slider.value]); };
  slider.addEventListener('input', showCount);
  showCount();
  // The population applies when the slider is let go: a new colony of that
  // many ants in new Workers (Restart starts the same population again).
  slider.addEventListener('change', () => { if (world) begin(); });
  // The rest applies at once, to every rest that starts from now on.
  const showRest = () => { restCount.textContent = duration(RESTS[restSlider.value]); };
  restSlider.addEventListener('input', showRest);
  restSlider.addEventListener('change', () => world?.workers.forEach((worker) => worker.postMessage({ rest: RESTS[restSlider.value] })));
  showRest();

  const sprites = await atlas(await (await fetch('/assets/brand/ant.svg')).text(), colors.colony);
  const paint = overlay.getContext('2d');

  function begin() {
    world?.workers.forEach((worker) => worker.terminate()); // their memory goes with them: a WebAssembly memory never shrinks
    world?.renderer.destroy?.();
    world?.canvas.remove();
    Object.values(costs).forEach((c) => c.clear());
    const population = POPULATIONS[slider.value];
    costs.actors.setLimit(population);
    costs.speed.setLimit(1, 'real time');
    let canvas = el('canvas', { class: 'anthill__ants', width: WIDTH, height: HEIGHT, 'aria-hidden': 'true' });
    let renderer = createRenderer(canvas, population, sprites, colors.groundHex);
    if (!renderer) { // a canvas that tried WebGL takes no 2D context: the dots get a fresh one
      canvas = el('canvas', { class: 'anthill__ants', width: WIDTH, height: HEIGHT, 'aria-hidden': 'true' });
      renderer = dots(canvas, population, colors);
    }
    stage.prepend(canvas);
    // A Worker per colony, each with half the room, joined by a channel
    // they trade frames over.
    const workers = HILLS.map(() => new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' }));
    world = {
      workers, canvas, renderer, population, ready: false, stats: null, speed: 1, messages: 0, perMessage: null, added: null, sim: 0, wall: 0, memory: null,
      parts: workers.map(() => ({ progress: 0, ready: false, data: null })),
      antSize: Math.max(2.5, 16 * (1000 / population) ** 0.25),
    };
    const current = world;
    status.textContent = `Hatching ${number.format(population)} ants…`;
    const channel = new MessageChannel();
    const seed = Math.floor(Math.random() * 2 ** 31);
    workers.forEach((worker, colony) => {
      const part = current.parts[colony];
      worker.onmessage = ({ data }) => {
        if (current !== world) return;
        if (data.progress !== undefined) {
          part.progress = data.progress;
          const done = current.parts.reduce((sum, p) => sum + p.progress, 0) / current.parts.length;
          status.textContent = `Hatching ${number.format(population)} ants… ${Math.round(done * 100)}%`;
        }
        if (data.ready) { part.ready = true; part.base = data.base; current.ready = current.parts.every((p) => p.ready); }
        if (data.error) status.textContent = `The simulation stopped: ${data.error.split('\n')[0]}`;
        if (data.delta) apply(current, part, data);
      };
      worker.onerror = (event) => { status.textContent = `The simulation stopped: ${event.message}`; };
      const port = colony === 0 ? channel.port1 : channel.port2;
      worker.postMessage({ start: true, population: population / 2, colony, seed, offset: colony * (population / 2), rest: RESTS[restSlider.value], port }, [port]);
    });
  }
  restart.addEventListener('click', begin);

  // ---- the view: the field seen at `scale`, centred on (cx, cy)
  const MAX_ZOOM = 32;
  const view = {
    scale: 1, cx: WIDTH / 2, cy: HEIGHT / 2,
    get left() { return this.cx - WIDTH / 2 / this.scale; },
    get top() { return this.cy - HEIGHT / 2 / this.scale; },
  };
  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
  function settle() {
    view.cx = clamp(view.cx, WIDTH / 2 / view.scale, WIDTH - WIDTH / 2 / view.scale);
    view.cy = clamp(view.cy, HEIGHT / 2 / view.scale, HEIGHT - HEIGHT / 2 / view.scale);
    zoomLevel.textContent = `${view.scale < 10 ? view.scale.toFixed(1).replace(/\.0$/, '') : Math.round(view.scale)}×`;
    zoomOut.disabled = view.scale <= 1;
    zoomIn.disabled = view.scale >= MAX_ZOOM;
    zoomFit.disabled = view.scale <= 1;
    overlay.classList.toggle('anthill__overlay--zoomed', view.scale > 1);
  }
  /** Zooms to `scale`, keeping the field point (fx, fy) where it is on the screen. */
  function zoomTo(scale, fx = view.cx, fy = view.cy) {
    const ux = (fx - view.left) / (WIDTH / view.scale), uy = (fy - view.top) / (HEIGHT / view.scale);
    view.scale = clamp(scale, 1, MAX_ZOOM);
    view.cx = fx + (0.5 - ux) * WIDTH / view.scale;
    view.cy = fy + (0.5 - uy) * HEIGHT / view.scale;
    settle();
  }
  function panBy(dx, dy) { view.cx += dx; view.cy += dy; settle(); }
  /** The field point under a pointer. */
  function toField(clientX, clientY) {
    const box = overlay.getBoundingClientRect();
    return [view.left + ((clientX - box.left) / box.width) * (WIDTH / view.scale), view.top + ((clientY - box.top) / box.height) * (HEIGHT / view.scale)];
  }
  zoomIn.addEventListener('click', () => zoomTo(view.scale * 2));
  zoomOut.addEventListener('click', () => zoomTo(view.scale / 2));
  zoomFit.addEventListener('click', () => zoomTo(1));
  settle();

  // A tap drops food or squashes ants; a drag moves the view when zoomed
  // in; two fingers pinch and move it (below); Ctrl or Cmd with the wheel
  // zooms at the cursor (the wheel alone scrolls the page).
  const pointers = new Map();
  let dragged = false;
  overlay.addEventListener('pointerdown', (event) => {
    overlay.setPointerCapture(event.pointerId);
    pointers.set(event.pointerId, { x: event.clientX, y: event.clientY, startX: event.clientX, startY: event.clientY });
    if (pointers.size === 1) dragged = false;
  });
  overlay.addEventListener('pointermove', (event) => {
    const pointer = pointers.get(event.pointerId);
    if (!pointer) return;
    const box = overlay.getBoundingClientRect();
    if (pointers.size > 1) { dragged = true; return; } // a pinch: the touch handlers have it
    if (!dragged && Math.hypot(event.clientX - pointer.startX, event.clientY - pointer.startY) < 5) return;
    dragged = true;
    if (view.scale > 1) panBy(-((event.clientX - pointer.x) / box.width) * (WIDTH / view.scale), -((event.clientY - pointer.y) / box.height) * (HEIGHT / view.scale));
    pointer.x = event.clientX; pointer.y = event.clientY;
  });
  const release = (event) => {
    if (!pointers.delete(event.pointerId)) return;
    if (event.type === 'pointerup' && pointers.size === 0 && !dragged && world?.ready) {
      const [x, y] = toField(event.clientX, event.clientY);
      // Food goes to one colony, which hands it to the other for the same
      // frame; a squash goes to both, each killing its own ants.
      if (tool === 'food') world.workers[0].postMessage({ food: true, x, y });
      else world.workers.forEach((worker) => worker.postMessage({ squash: true, x, y, radius: Math.max(4, 16 / Math.sqrt(view.scale)) }));
    }
  };
  overlay.addEventListener('pointerup', release);
  overlay.addEventListener('pointercancel', release);
  // Two fingers zoom and move the view. Touch events, not pointer events:
  // only cancelling a touchmove keeps every phone's browser from zooming the
  // page instead (Safari does not leave a pinch to the page for
  // touch-action alone).
  let pinch = null; // the last two-finger frame: {x, y, distance}
  const twoFingers = (touches) => ({
    x: (touches[0].clientX + touches[1].clientX) / 2, y: (touches[0].clientY + touches[1].clientY) / 2,
    distance: Math.hypot(touches[0].clientX - touches[1].clientX, touches[0].clientY - touches[1].clientY) || 1,
  });
  overlay.addEventListener('touchstart', (event) => {
    if (event.touches.length !== 2) return;
    event.preventDefault();
    pinch = twoFingers(event.touches);
    dragged = true;
  }, { passive: false });
  overlay.addEventListener('touchmove', (event) => {
    if (event.touches.length !== 2) return;
    event.preventDefault();
    const now = twoFingers(event.touches);
    if (pinch) {
      const [fx, fy] = toField(pinch.x, pinch.y);
      zoomTo(view.scale * (now.distance / pinch.distance), fx, fy); // the point between the fingers stays under them
      const [gx, gy] = toField(now.x, now.y);
      panBy(fx - gx, fy - gy); // and follows them as they move
    }
    pinch = now;
  }, { passive: false });
  const lift = (event) => { if (event.touches.length < 2) pinch = null; };
  overlay.addEventListener('touchend', lift);
  overlay.addEventListener('touchcancel', lift);
  overlay.addEventListener('gesturestart', (event) => event.preventDefault()); // Safari's own pinch

  overlay.addEventListener('wheel', (event) => {
    if (!event.ctrlKey && !event.metaKey) return;
    event.preventDefault();
    const [fx, fy] = toField(event.clientX, event.clientY);
    zoomTo(view.scale * Math.exp(-event.deltaY * 0.002), fx, fy);
  }, { passive: false });
  overlay.addEventListener('keydown', (event) => {
    const step = 0.1 * WIDTH / view.scale;
    const keys = {
      '+': () => zoomTo(view.scale * 2), '=': () => zoomTo(view.scale * 2), '-': () => zoomTo(view.scale / 2), '0': () => zoomTo(1),
      ArrowLeft: () => panBy(-step, 0), ArrowRight: () => panBy(step, 0), ArrowUp: () => panBy(0, -step), ArrowDown: () => panBy(0, step),
    };
    if (!keys[event.key]) return;
    event.preventDefault();
    keys[event.key]();
  });

  // Each colony reports its own frames; the field shows both, at the time
  // of the one behind, which the other waits for.
  function apply(state, part, data) {
    state.renderer.apply(data.delta);
    part.data = data;
    const parts = state.parts.map((p) => p.data);
    if (parts.some((p) => !p)) return;
    const behind = parts.reduce((a, b) => (b.stats.time < a.stats.time ? b : a));
    const stats = {
      time: behind.stats.time,
      colonies: parts.map((p) => p.stats.colony),
      fights: parts.reduce((sum, p) => sum + p.stats.fights, 0),
      food: parts[0].stats.food,
    };
    const memory = parts.every((p) => p.memory) ? parts.reduce((sum, p) => sum + p.memory, 0) : null;
    Object.assign(state, {
      stats, memory, sim: behind.stats.time, wall: behind.wall,
      speed: Math.min(...parts.map((p) => p.speed)),
      messages: parts.reduce((sum, p) => sum + p.messages, 0),
      // both colonies' time over both colonies' messages
      perMessage: parts.every((p) => p.perMessage !== null && p.messages > 0)
        ? parts.reduce((sum, p) => sum + p.perMessage * p.messages, 0) / parts.reduce((sum, p) => sum + p.messages, 0) : null,
      added: parts.every((p, i) => p.memory && typeof state.parts[i].base === 'number')
        ? parts.reduce((sum, p, i) => sum + p.memory - state.parts[i].base, 0) : null,
    });
    if (!state.ready) return;
    const ants = stats.colonies.reduce((sum, c) => sum + c.workers + c.brood, 0);
    hills.replaceChildren(...stats.colonies.map((c, i) => el('span', { class: `anthill__hill anthill__hill--${i + 1}` },
      `Hill ${i + 1}: ${number.format(c.workers)} workers (${number.format(c.out)} out), ${number.format(c.brood)} brood, ${number.format(c.stored)} food home`)));
    // What the ants added to the module's memory, over the ants: the module
    // and the empty hosts left out, which a small colony would make look large.
    const perAnt = state.added ? ` · ${number.format(Math.round(state.added / ants))} bytes of WebAssembly memory an ant` : '';
    status.textContent = `${number.format(ants + 2)} actors (2 queens) · ${number.format(stats.fights)} fights · ${Math.floor(stats.time)} s simulated${perAnt}`;
  }

  // Once a second, each cost chart takes its latest value.
  let frames = 0;
  setInterval(() => {
    const state = world;
    if (!state?.ready) { frames = 0; return; }
    costs.actors.push(state.stats.colonies.reduce((sum, c) => sum + c.workers + c.brood, 0) + 2);
    costs.fps.push(frames);
    costs.speed.push(state.speed);
    if (state.memory) costs.memory.push(state.memory);
    costs.messages.push(state.messages);
    if (state.perMessage !== null) costs.perMessage.push(state.perMessage);
    frames = 0;
  }, 1000);

  function draw() {
    requestAnimationFrame(draw);
    frames++;
    const state = world;
    if (state?.ready) {
      fit(state.canvas, state.renderer.kind === 'webgl');
      // The simulation's time now: the last frame's, moved on by the wall
      // clock at the simulation's speed, never past the next frame.
      const t = state.sim + Math.min(0.1, ((performance.now() - state.wall) / 1000) * state.speed);
      state.renderer.draw(t, state.antSize, view);
    }
    paint.setTransform(1, 0, 0, 1, 0, 0);
    paint.clearRect(0, 0, WIDTH, HEIGHT);
    paint.setTransform(view.scale, 0, 0, view.scale, -view.left * view.scale, -view.top * view.scale);
    // A source's area is what is left of it, a hill's its colony against
    // the colony's room: both shrink as they empty.
    for (const [x, y, left, held] of state?.stats?.food ?? []) {
      if (left <= 0) continue;
      paint.fillStyle = colors.food;
      paint.beginPath(); paint.arc(x, y, Math.max(1.5, FOOD_RADIUS * Math.sqrt(left / held)), 0, Math.PI * 2); paint.fill();
    }
    HILLS.forEach((hill, i) => {
      const colony = state?.stats?.colonies[i];
      const share = colony && state.ready ? Math.min(1, (colony.workers + colony.brood) / (state.population / 2)) : 1;
      paint.fillStyle = colors.hill[i];
      paint.strokeStyle = colors.ink;
      paint.lineWidth = 1.5 / view.scale;
      paint.beginPath(); paint.arc(hill.x, hill.y, Math.max(4, HILL_RADIUS * Math.sqrt(share)), 0, Math.PI * 2); paint.fill(); paint.stroke();
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

/** A WebGL canvas draws at the screen's pixels; the 2D fallback at the field's. */
function fit(canvas, sharp) {
  if (!sharp) return;
  const width = Math.round(canvas.clientWidth * devicePixelRatio), height = Math.round(canvas.clientHeight * devicePixelRatio);
  if (width && (canvas.width !== width || canvas.height !== height)) { canvas.width = width; canvas.height = height; }
}

// Where WebGL2 is missing: a block of pixels per ant, at least one screen
// pixel wide, on a 2D canvas at the field's resolution.
function dots(canvas, population, colors) {
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

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
