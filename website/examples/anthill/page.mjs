// The anthill's page: the field, a population slider, two tools and the
// live costs. Each colony runs in a worker.mjs of its own; renderer.mjs draws every ant
// with the design system's rigged ant (sprites.mjs) on the GPU, and where
// WebGL2 is missing, this draws a dot per ant on a 2D canvas instead.
// Hills and food are drawn on a 2D canvas above the ants. A reader may
// follow one ant: the view keeps it in the middle, and follow.mjs shows its
// chart beside it, lit in the states its host reports.
import { WIDTH, HEIGHT, HILLS, HILL_RADIUS } from './world.mjs';
import { atlas } from './sprites.mjs';
import { createRenderer } from './renderer.mjs';
import { launch, ants as antsOf, now, fit, dots, palette, el } from './field.mjs';
import { chart } from '/assets/components/costs.mjs';

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
const FOLLOW_ZOOM = 8; // the least zoom while following, so the ant is seen

export async function mount(root) {
  const colors = palette(root);
  const slider = el('input', { type: 'range', min: 0, max: POPULATIONS.length - 1, step: 1, value: DEFAULT, 'aria-label': 'Population' });
  const count = el('output', { class: 'anthill__count' });
  const restSlider = el('input', { type: 'range', min: 0, max: RESTS.length - 1, step: 1, value: REST, 'aria-label': 'Rest between outings' });
  const restCount = el('output', { class: 'anthill__count' });
  const restart = el('button', { type: 'button', class: 'ta-btn ta-btn--primary' }, 'Restart');
  const tools = el('div', { role: 'radiogroup', class: 'ta-tabs ta-tabs--segmented', 'aria-label': 'Tool' });
  let tool = 'food';
  for (const [name, label] of [['food', 'Drop food'], ['squash', 'Squash ants'], ['follow', 'Follow an ant']]) {
    const button = el('button', { type: 'button', role: 'radio', class: 'ta-tab', 'aria-checked': String(name === tool) }, label);
    button.addEventListener('click', () => {
      tool = name;
      for (const other of tools.children) other.setAttribute('aria-checked', String(other === button));
      if (name === 'follow' && !world?.follow) followAny(); // a tap on the field picks another
    });
    tools.append(button);
  }
  const zoomOut = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary', 'aria-label': 'Zoom out' }, '−');
  const zoomIn = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary', 'aria-label': 'Zoom in' }, '+');
  const zoomFit = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary' }, 'Fit');
  const zoomLevel = el('output', { class: 'anthill__zoom-level', 'aria-live': 'polite' });
  const zoom = el('div', { class: 'anthill__zoom', role: 'group', 'aria-label': 'Zoom' }, zoomOut, zoomLevel, zoomIn, zoomFit);
  const overlay = el('canvas', { class: 'anthill__overlay', width: WIDTH, height: HEIGHT, tabindex: '0',
    'aria-label': 'Two anthills. Select to drop food, squash ants or follow an ant; drag to move when zoomed in; plus, minus and 0 zoom; arrow keys move; Escape stops following.' });
  const stage = el('div', { class: 'anthill__stage' }, overlay);
  const scene = el('div', { class: 'anthill__scene' }, stage);
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
    scene, hills, status,
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

  // ---- following one ant. Its colony's worker reports it after every
  // frame (world.mjs, inspect()); the panel loads with the first follow.
  let panel = null;
  let requests = 0; // pick requests, so a late answer to an old one is ignored
  async function showPanel() {
    if (panel) return panel;
    const [{ followPanel }, text] = await Promise.all([import('./follow.mjs'),
      fetch(new URL('./ant.scxml', import.meta.url)).then((response) => response.text())]);
    panel ??= followPanel(text, { onStop: stopFollowing, onAnother: followAny });
    if (!panel.element.isConnected) scene.append(panel.element);
    return panel;
  }
  /** Asks the colonies for an ant: near (x, y), or any when x is null. */
  function pick(x = null, y = null) {
    const state = world;
    if (!state?.ready) return;
    const request = ++requests;
    const radius = Math.max(3, 24 / Math.sqrt(view.scale));
    // Any ant: one colony, the other when it has none. Near a point: both, the nearer wins.
    const colonies = x === null ? [Math.random() < 0.5 ? 0 : 1] : [0, 1];
    state.picks = { request, answers: [], expected: colonies.length, any: x === null, tried: colonies };
    colonies.forEach((c) => state.workers[c].postMessage({ pick: true, any: x === null, x, y, radius, request }));
  }
  function picked(state, data) {
    const picks = state.picks;
    if (!picks || data.request !== picks.request) return;
    picks.answers.push(data.picked);
    if (picks.answers.length < picks.expected) return;
    const best = picks.answers.filter(Boolean).sort((a, b) => (a.distance ?? 0) - (b.distance ?? 0))[0];
    state.picks = null;
    if (best) startFollowing(best.slot);
    else if (picks.any && picks.tried.length === 1) { // that colony had no worker: the other
      const other = 1 - picks.tried[0];
      state.picks = { ...picks, answers: [], tried: [...picks.tried, other] };
      state.workers[other].postMessage({ pick: true, any: true, request: picks.request });
    }
  }
  function followAny() { pick(); }
  async function startFollowing(slot) {
    const state = world;
    const colony = slot < state.population / 2 ? 0 : 1;
    state.workers.forEach((worker, c) => worker.postMessage({ follow: c === colony ? slot : null }));
    state.follow = { slot, colony, report: null };
    if (view.scale < FOLLOW_ZOOM) zoomTo(FOLLOW_ZOOM);
    (await showPanel()).follow(slot, colony);
  }
  function stopFollowing() {
    world?.workers.forEach((worker) => worker.postMessage({ follow: null }));
    if (world) world.follow = null;
    panel?.hide();
  }
  function followed(state, report) {
    if (!state.follow || report.slot !== state.follow.slot) return;
    if (report.ended) state.follow.ended = true; // the view stays where it died
    else state.follow.report = report;
    panel?.show(report);
  }

  function begin() {
    stopFollowing();
    world?.stop(); // their memory goes with them: a WebAssembly memory never shrinks
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
    status.textContent = `Hatching ${number.format(population)} ants…`;
    const current = world = launch({
      population, rest: RESTS[restSlider.value], renderer,
      onProgress: (done) => { status.textContent = `Hatching ${number.format(population)} ants… ${Math.round(done * 100)}%`; },
      onError: (error) => { status.textContent = `The simulation stopped: ${error}`; },
      onFrame: (state) => show(state),
      onMessage: (data) => {
        if (data.picked !== undefined) picked(current, data);
        if (data.followed) followed(current, data.followed);
      },
    });
    Object.assign(current, { canvas, follow: null, picks: null, antSize: Math.max(2.5, 16 * (1000 / population) ** 0.25) });
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
      if (tool === 'follow') pick(x, y);
      else if (tool === 'food') world.workers[0].postMessage({ food: true, x, y });
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
      Escape: () => stopFollowing(),
      ArrowLeft: () => panBy(-step, 0), ArrowRight: () => panBy(step, 0), ArrowUp: () => panBy(0, -step), ArrowDown: () => panBy(0, step),
    };
    if (!keys[event.key]) return;
    event.preventDefault();
    keys[event.key]();
  });

  // The hills' and the field's numbers, after every frame of both colonies.
  function show(state) {
    if (!state.ready) return;
    const { stats } = state;
    const ants = antsOf(state);
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
    costs.actors.push(antsOf(state) + 2);
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
      const t = now(state);
      // A followed ant stays in the middle: where its trip puts it now.
      const trip = state.follow?.report?.trip;
      if (trip) {
        const [fx, fy] = along(trip, t);
        view.cx = fx; view.cy = fy;
        settle();
      }
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
    // A ring around the followed ant, dashed while it is inside the hill.
    const report = state?.follow?.report;
    if (report && !state.follow.ended) {
      const [fx, fy] = along(report.trip, now(state));
      paint.strokeStyle = colors.ink;
      paint.lineWidth = 2 / view.scale;
      paint.setLineDash(report.outside ? [] : [4 / view.scale, 3 / view.scale]);
      paint.beginPath(); paint.arc(fx, fy, Math.max(3, state.antSize * 0.9), 0, Math.PI * 2); paint.stroke();
      paint.setLineDash([]);
    }
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

/** Where a trip [x0, y0, x1, y1, t0, t1] puts its ant at time t. */
function along([x0, y0, x1, y1, t0, t1], t) {
  const f = t1 > t0 ? Math.min(1, Math.max(0, (t - t0) / (t1 - t0))) : 1;
  return [x0 + (x1 - x0) * f, y0 + (y1 - y0) * f];
}
