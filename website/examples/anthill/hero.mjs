// The anthill as the home page's picture: the same two colonies as the full
// demo (page.mjs), with no controls. The field starts when it comes into
// view, holds still while it is out of view or its tab is hidden, and under
// prefers-reduced-motion shows one still frame. One line under it says what
// the ants are, live: how many actors run in this tab, and the WebAssembly
// memory they take. The field and the line link to the full demo.
import { WIDTH, HEIGHT, HILLS, HILL_RADIUS } from './world.mjs';
import { atlas } from './sprites.mjs';
import { createRenderer } from './renderer.mjs';
import { launch, ants, now, fit, dots, palette, el } from './field.mjs';

// A population a phone keeps up with: see README.md, "The home page's field".
const POPULATION = 20_000;
// Short rests, so most of the colony is out on the field at any time.
const REST = 10;
// Ants a little larger than the full demo's at this population: the field
// is drawn at half its width.
const ANT_SIZE = 9;
// Under reduced motion, the simulated seconds run before the still frame:
// long enough for the workers to leave the hills.
const STILL = 8;
const FOOD_RADIUS = 12;
const DEMO = '/anthill/';
const number = new Intl.NumberFormat('en');

export async function mount(root) {
  const colors = palette(root);
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const overlay = el('canvas', { class: 'hero-field__overlay', width: WIDTH, height: HEIGHT });
  // The field repeats the line's link for a pointer; the line's is the one a keyboard and a screen reader reach.
  const stage = el('a', { class: 'hero-field__stage', href: DEMO, tabindex: '-1', 'aria-hidden': 'true' }, overlay);
  const count = el('span', { class: 'hero-field__number' });
  const memory = el('span', { class: 'hero-field__number' });
  const said = el('span', {}, 'These ants are ', count, ' actors running in this tab, in ', memory, ' of WebAssembly memory.');
  const line = el('p', { class: 'hero-field__line' }, said, ' ', el('a', { href: DEMO }, 'Watch a million'));
  said.hidden = true;
  root.classList.add('hero-field');
  root.append(stage, line);

  const sprites = await atlas(await (await fetch('/assets/brand/ant.svg')).text(), colors.colony);
  const paint = overlay.getContext('2d');
  const view = { left: 0, top: 0, scale: 1 };
  let field = null, still = false, frame = 0;

  function start() {
    let canvas = el('canvas', { class: 'hero-field__ants', width: WIDTH, height: HEIGHT });
    let renderer = createRenderer(canvas, POPULATION, sprites, colors.groundHex);
    if (!renderer) { // a canvas that tried WebGL takes no 2D context: the dots get a fresh one
      canvas = el('canvas', { class: 'hero-field__ants', width: WIDTH, height: HEIGHT });
      renderer = dots(canvas, POPULATION, colors);
    }
    stage.prepend(canvas);
    field = launch({
      population: POPULATION, rest: REST, renderer,
      onFrame: (state) => {
        if (!state.ready) return;
        if (reduced && !still && state.sim >= STILL) { still = true; state.pause(true); draw(); }
        say(state);
      },
      onError: () => { stage.classList.remove('hero-field__stage--live'); },
    });
    Object.assign(field, { canvas });
  }

  // The line changes at most once a second, so it reads as a reading, not a counter.
  let saidAt = 0;
  function say(state) {
    if (performance.now() - saidAt < 1000 && !said.hidden) return;
    saidAt = performance.now();
    count.textContent = number.format(ants(state) + 2); // and the two queens
    memory.textContent = state.memory ? `${number.format(Math.round(state.memory / 1048576))} MB` : '…';
    said.hidden = false;
  }

  function draw() {
    const state = field;
    if (!state?.ready || (reduced && !still)) return;
    stage.classList.add('hero-field__stage--live');
    fit(state.canvas, state.renderer.kind === 'webgl');
    state.renderer.draw(reduced ? state.sim : now(state), ANT_SIZE, view);
    paint.clearRect(0, 0, WIDTH, HEIGHT);
    for (const [x, y, left, held] of state.stats?.food ?? []) {
      if (left <= 0) continue;
      paint.fillStyle = colors.food;
      paint.beginPath(); paint.arc(x, y, Math.max(1.5, FOOD_RADIUS * Math.sqrt(left / held)), 0, Math.PI * 2); paint.fill();
    }
    HILLS.forEach((hill, i) => {
      const colony = state.stats?.colonies[i];
      const share = colony ? Math.min(1, (colony.workers + colony.brood) / (POPULATION / 2)) : 1;
      paint.fillStyle = colors.hill[i];
      paint.strokeStyle = colors.ink;
      paint.lineWidth = 1.5;
      paint.beginPath(); paint.arc(hill.x, hill.y, Math.max(4, HILL_RADIUS * Math.sqrt(share)), 0, Math.PI * 2); paint.fill(); paint.stroke();
    });
  }

  // Runs only while the field is on the screen and its tab is shown.
  let seen = false;
  function loop() {
    frame = 0;
    if (!running()) return;
    draw();
    frame = requestAnimationFrame(loop);
  }
  const running = () => seen && document.visibilityState === 'visible' && !still;
  function update() {
    if (running()) {
      if (!field) start();
      else field.pause(false);
      if (!frame) frame = requestAnimationFrame(loop);
    } else if (field && !still) {
      field.pause(true);
    }
  }
  new IntersectionObserver(([entry]) => { seen = entry.isIntersecting; update(); }).observe(stage);
  document.addEventListener('visibilitychange', update);
  // A still frame is drawn again when the field changes size.
  new ResizeObserver(() => { if (still) draw(); }).observe(stage);
}
