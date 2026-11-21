// The ants, drawn: one fixed canvas behind the page's content. Each ant
// is the design system's rigged ant (ant.svg), its parts as Path2D objects,
// posed by the ant-animation skill's gait (ant.mjs: the feet planted, the
// stride from the speed). The server sends trips (two points, two times,
// as fractions of the field); here each ant steers after the point its
// trip is at, in this viewport's pixels, so no ant is ever stretched.
// Also drawn, faintly: this visitor's country's trail and the food.
import { makeAnt, advance } from './ant.mjs';

const LENGTH = 26;              // an ant's body, px
const MAX = 60;                 // ants drawn; this tab's always among them
const FPS = 30;
const INK = '#302B28', PINE = '#255E45', HIGHLIGHT = '#D5DCD7'; // the SVG's own colours
const HIT = 22;                 // px around an ant that a tap reaches
const VMAX = 130;               // px/s an ant may hurry to catch its trip (a greeting's walk is twice a stroll)
const GREET_GAP = 1.15;         // body lengths between two greeting ants' centres: head to head, antennae crossed

export function createLayer({ svgText, flagURL = (cc) => `/vendor/flag-icons/flags/4x3/${cc.toLowerCase()}.svg`, onTap }) {
  const canvas = document.createElement('canvas');
  canvas.className = 'ants';
  canvas.setAttribute('aria-hidden', 'true');
  document.body.prepend(canvas);
  const context = canvas.getContext('2d');
  const rig = buildRig(svgText);
  const scale = LENGTH / 633; // the rig's body, mandibles to gaster tip, is 633 units
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  const ants = new Map(); // handle -> {body, trip, country, asleep, leaving, greetUntil}
  const flags = new Map(); // country -> Image | null
  const bitten = new Map(); // pile -> until when it shakes
  const bite = (id) => bitten.set(id, performance.now() + 400);
  let target = 0; // the ant this tab's ant walks to, while the server says so
  let you = 0, offset = 0, field = null, running = false, shown = false, last = 0, width = 0, height = 0, dpr = 1;
  const colors = { ink: INK, pine: PINE, trail: PINE };

  function resize() {
    dpr = Math.min(2, devicePixelRatio || 1);
    width = innerWidth; height = innerHeight;
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
    const style = getComputedStyle(document.documentElement);
    colors.ink = style.getPropertyValue('--fg-1').trim() || INK;
    colors.pine = style.getPropertyValue('--accent').trim() || PINE;
    colors.trail = colors.pine;
  }
  addEventListener('resize', resize);
  resize();

  const serverNow = () => performance.now() + offset;
  const px = (x, y) => [x * width, y * height];

  function at(trip, t) {
    const [, x0, y0, t0, x1, y1, t1] = trip;
    const f = t1 > t0 ? Math.min(1, Math.max(0, (t - t0) / (t1 - t0))) : 1;
    return px(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f);
  }

  function add(handle, country, asleep, trip) {
    const [x, y] = at(trip, serverNow());
    const body = makeAnt({ x, y, heading: Math.random() * 360, scale, seed: handle });
    body.alpha = body.target.alpha = 0; // fades in
    body.target.alpha = 1;
    if (asleep) { body.target.tuck = 1; body.tuck = 1; body.target.alpha = 0.45; }
    advance(body, 0);
    ants.set(handle, { body, trip, country, asleep, leaving: false, greetUntil: 0 });
    if (country && !flags.has(country)) loadFlag(country);
  }

  function loadFlag(country) {
    flags.set(country, null);
    const image = new Image();
    image.onload = () => flags.set(country, image);
    image.src = flagURL(country);
  }

  // ---- what the server says
  function welcome(message) {
    offset = message.now - performance.now();
    you = message.you;
    ants.clear();
    for (const [handle, country, asleep, trip] of message.ants) add(handle, country, asleep, trip);
    field = message.field;
  }
  function apply(message) {
    switch (message.t) {
      case 'join': if (!ants.has(message.h)) add(message.h, message.country, false, [message.h, 0.5, 0.5, 0, 0.5, 0.5, 0]); break;
      case 'trip': {
        const ant = ants.get(message.trip[0]);
        if (ant) { ant.trip = message.trip; if (ant.body.target.greet && !message.trip[7]) ant.body.target.greet = 0; }
        break;
      }
      case 'greet': greet(ants.get(message.a), ants.get(message.b)); break;
      case 'sleep': {
        const ant = ants.get(message.h);
        if (ant) { ant.asleep = message.asleep; ant.body.target.tuck = message.asleep ? 1 : 0; ant.body.target.alpha = message.asleep ? 0.45 : 1; }
        break;
      }
      case 'leave': { const ant = ants.get(message.h); if (ant) ant.leaving = true; break; }
      case 'eat': { const ant = ants.get(message.h); if (ant) { ant.eatUntil = performance.now() + 2500; ant.body.target.eat = 1; } bite(message.food); break; }
      case 'target': target = message.h; break;
      case 'food': if (field) field.food = message.food; else field = { food: message.food }; break;
    }
  }

  // ---- a greeting: the server stops both ants where they are, in fractions
  // of the field, which each screen stretches its own way. Here, in this
  // screen's pixels, the two take their places: head to head on the line
  // between them, GREET_GAP body lengths apart, about their midpoint.
  function greet(a, b) {
    for (const ant of [a, b]) if (ant) { ant.greetUntil = performance.now() + 3000; ant.body.target.greet = 1; }
    if (!a || !b) return;
    const mx = (a.body.x + b.body.x) / 2, my = (a.body.y + b.body.y) / 2;
    let dx = b.body.x - a.body.x, dy = b.body.y - a.body.y, d = Math.hypot(dx, dy);
    if (d < 1) { const h = (a.body.heading * Math.PI) / 180; dx = Math.sin(h); dy = -Math.cos(h); d = 1; } // on top of each other: along a's heading
    const half = (GREET_GAP * LENGTH) / 2, ux = dx / d, uy = dy / d;
    a.spot = [mx - ux * half, my - uy * half];
    b.spot = [mx + ux * half, my + uy * half];
    a.faceAt = b.spot; b.faceAt = a.spot;
  }

  // ---- steering: after the point the ant's trip is at, now
  function steer(ant, dt, t) {
    const a = ant.body;
    let [tx, ty] = at(ant.trip, t);
    if (ant.leaving) { // off the nearest edge
      const edges = [[-60, a.y], [width + 60, a.y], [a.x, -60], [a.x, height + 60]];
      [tx, ty] = edges.sort((p, q) => Math.hypot(p[0] - a.x, p[1] - a.y) - Math.hypot(q[0] - a.x, q[1] - a.y))[0];
      a.target.alpha = 0;
    }
    const dx = tx - a.x, dy = ty - a.y, distance = Math.hypot(dx, dy);
    if (ant.greetUntil && performance.now() > ant.greetUntil) { ant.greetUntil = 0; a.target.greet = 0; ant.spot = ant.faceAt = null; }
    if (ant.spot && !ant.asleep) { // greeting: face the other ant, and step forward or back onto its place
      const [fx, fy] = ant.faceAt;
      const wanted = (Math.atan2(fx - a.x, -(fy - a.y)) * 180) / Math.PI;
      const error = ((wanted - a.heading + 540) % 360) - 180;
      const h = (a.heading * Math.PI) / 180;
      const along = (ant.spot[0] - a.x) * Math.sin(h) - (ant.spot[1] - a.y) * Math.cos(h);
      a.target.w = Math.max(-160, Math.min(160, error * 4));
      a.target.v = Math.abs(error) < 60 ? Math.max(-25, Math.min(40, along * 2.5)) : 0;
      a.target.idle = 0;
      // the rest of the offset, sideways, closes without walking
      const side = Math.hypot(ant.spot[0] - a.x, ant.spot[1] - a.y);
      if (side > 0.5 && Math.abs(error) < 20) { a.x += (ant.spot[0] - a.x) * Math.min(1, dt * 1.5) * 0.5; a.y += (ant.spot[1] - a.y) * Math.min(1, dt * 1.5) * 0.5; }
      advance(a, dt);
      return;
    }
    if (ant.eatUntil && performance.now() > ant.eatUntil) { ant.eatUntil = 0; a.target.eat = 0; }
    const face = ant.trip[7];
    let wanted = null;
    if (distance > 1.5 && !ant.asleep) wanted = (Math.atan2(dx, -dy) * 180) / Math.PI;
    else if (face && !ant.asleep) { const [fx, fy] = px(face[0], face[1]); wanted = (Math.atan2(fx - a.x, -(fy - a.y)) * 180) / Math.PI; }
    const error = wanted === null ? 0 : ((wanted - a.heading + 540) % 360) - 180;
    a.target.w = ant.asleep ? 0 : Math.max(-140, Math.min(140, error * 3));
    // Walk when roughly facing the point; turn in place when it is behind.
    const speed = ant.leaving ? VMAX : Math.min(VMAX, distance * 1.8);
    a.target.v = ant.asleep || distance < 1.5 ? 0 : speed * Math.max(0, Math.cos((error * Math.PI) / 180));
    a.target.idle = !ant.asleep && distance < 1.5 && !ant.greetUntil && !ant.eatUntil ? 0.6 : 0;
    advance(a, dt);
  }

  // ---- drawing
  function frame(time) {
    if (!running) return;
    requestAnimationFrame(frame);
    if (time - last < 1000 / FPS - 1) return;
    const dt = Math.min(0.1, (time - (last || time)) / 1000);
    last = time;
    const t = serverNow();
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    context.clearRect(0, 0, width, height);
    drawField();
    drawTrack(time);
    let drawn = 0;
    const order = [...ants.entries()].sort(([h]) => (h === you ? -1 : 0));
    for (const [handle, ant] of order) {
      if (reduced.matches) { const [x, y] = at(ant.trip, ant.trip[6]); ant.body.x = x; ant.body.y = y; advance(ant.body, 0); }
      else steer(ant, dt, t);
      if (ant.leaving && ant.body.alpha < 0.02) { ants.delete(handle); continue; }
      if (drawn++ < MAX) draw(ant, handle === you);
    }
  }

  function drawField() {
    if (!field) return;
    context.save();
    context.globalAlpha = 1;
    if (field.trail) {
      context.fillStyle = colors.trail;
      const cw = width / field.cols, ch = height / field.rows;
      for (let i = 0; i < field.trail.length; i++) {
        const v = field.trail[i];
        if (v < 12) continue;
        context.globalAlpha = Math.min(0.18, v / 255 * 0.25);
        const x = (i % field.cols + 0.5) * cw, y = (Math.floor(i / field.cols) + 0.5) * ch;
        context.beginPath(); context.arc(x, y, 1.6, 0, Math.PI * 2); context.fill();
      }
    }
    // A pile: its scent, and a seed for each bite left; it shakes as it is
    // bitten and is gone when the server says it is finished.
    for (const [id, fx, fy, left] of field.food) {
      const [x, y] = px(fx, fy);
      const shake = Math.max(0, (bitten.get(id) ?? 0) - performance.now()) / 400;
      const radius = 60 * left + 14;
      const gradient = context.createRadialGradient(x, y, 0, x, y, radius);
      gradient.addColorStop(0, 'rgba(216, 166, 78, 0.16)');
      gradient.addColorStop(1, 'rgba(216, 166, 78, 0)');
      context.globalAlpha = 1;
      context.fillStyle = gradient;
      context.beginPath(); context.arc(x, y, radius, 0, Math.PI * 2); context.fill();
      context.fillStyle = 'rgba(184, 134, 46, 0.6)';
      const seeds = Math.max(1, Math.ceil(left * 9));
      for (let i = 0; i < seeds; i++) {
        const angle = i * 2.4 + id, r = 2.2 * Math.sqrt(i);
        const jitter = shake * Math.sin(performance.now() / 30 + i);
        context.beginPath(); context.ellipse(x + Math.cos(angle) * r + jitter, y + Math.sin(angle) * r, 1.6, 1.1, angle, 0, Math.PI * 2); context.fill();
      }
    }
    context.restore();
  }

  // The track to the ant this tab's ant walks to: a dotted line from its
  // head, the dots marching toward the target, and a dashed ring around it
  // (the motion study's trail, in the accent), until the server says the
  // walk ended.
  function drawTrack(time) {
    const mine = ants.get(you), other = ants.get(target);
    if (!target || !mine || !other || other.leaving) return;
    const a = mine.body, b = other.body;
    const h = (a.heading * Math.PI) / 180, ring = LENGTH * 0.8;
    const hx = a.x + Math.sin(h) * LENGTH * 0.55, hy = a.y - Math.cos(h) * LENGTH * 0.55;
    const dx = b.x - hx, dy = b.y - hy, d = Math.hypot(dx, dy);
    context.save();
    context.fillStyle = context.strokeStyle = colors.pine;
    const STEP = 9, march = reduced.matches ? 0 : ((time / 1000) * 18) % STEP;
    for (let s = STEP - march; s < d - ring - 2; s += STEP) {
      context.globalAlpha = 0.55 * Math.min(1, s / 30); // fades in from the ant
      context.beginPath(); context.arc(hx + (dx / d) * s, hy + (dy / d) * s, 1.8, 0, Math.PI * 2); context.fill();
    }
    context.globalAlpha = 0.6;
    context.lineWidth = 1.5;
    context.setLineDash([4, 4]);
    context.lineDashOffset = reduced.matches ? 0 : -(time / 1000) * 8;
    context.beginPath(); context.arc(b.x, b.y, ring, 0, Math.PI * 2); context.stroke();
    context.restore();
  }

  function draw(ant, mine) {
    const a = ant.body;
    context.save();
    context.globalAlpha = a.alpha * (mine ? 0.78 : 0.42);
    context.translate(a.x, a.y);
    context.rotate(((a.heading + a.sway) * Math.PI) / 180);
    context.scale(scale, scale);
    context.translate(-512, -530);
    const posed = joints(a.pose);
    for (const part of rig.parts) {
      context.save();
      for (const joint of part.chain) transform(posed[joint]);
      context.fillStyle = part.fill === INK ? colors.ink : part.fill === PINE ? colors.pine : part.fill;
      context.fill(part.path);
      if (part.gaster) flag(ant.country, part.path);
      context.restore();
    }
    if (a.eat > 0.3) { // a crumb held in the mandibles
      context.save();
      transform(posed.head);
      context.globalAlpha *= Math.min(1, (a.eat - 0.3) * 2);
      context.fillStyle = 'rgb(184, 134, 46)';
      context.beginPath(); context.ellipse(512, 196, 30, 22, 0, 0, Math.PI * 2); context.fill();
      context.restore();
    }
    context.restore();
  }
  function flag(country, path) {
    const image = country && flags.get(country);
    if (!image) return;
    context.save();
    context.clip(path);
    context.globalAlpha *= 0.85;
    context.drawImage(image, 430, 560, 165, 270); // the gaster's box, in rig units
    context.restore();
  }
  function transform(joint) {
    if (!joint) return;
    const [x, y] = joint.at;
    context.translate(x, y);
    context.rotate((joint.angle * Math.PI) / 180);
    if (joint.scale !== 1) context.scale(joint.scale, joint.scale);
    context.translate(-x, -y);
  }
  function joints(pose) {
    const fold = -38 * pose.tuck, m = 9 * pose.mandible;
    const j = {
      head: { at: rig.pivots.head, angle: pose.headYaw, scale: 1 - 0.03 * pose.tuck - (pose.headDip ?? 0) },
      gaster: { at: rig.pivots.gaster, angle: pose.gasterYaw, scale: 1 },
      petiole: { at: rig.pivots.petiole, angle: pose.petioleYaw, scale: 1 },
      'antenna-left': { at: rig.pivots['antenna-left'], angle: pose.antL + fold, scale: 1 },
      'antenna-right': { at: rig.pivots['antenna-right'], angle: -(pose.antR + fold), scale: 1 },
      'mandible-left': { at: rig.pivots['mandible-left'], angle: -m, scale: 1 },
      'mandible-right': { at: rig.pivots['mandible-right'], angle: m, scale: 1 },
    };
    for (const [id, leg] of Object.entries(pose.legs)) j[id] = { at: rig.pivots[id], angle: leg.angle, scale: leg.scale };
    return j;
  }

  // ---- a tap on another ant: outside links, buttons, fields and selected text
  addEventListener('pointerup', (event) => {
    if (!shown || event.button > 0) return;
    if (event.target.closest?.('a, button, input, textarea, select, label, summary, [contenteditable], [popover], pre, code')) return;
    if (String(getSelection?.() ?? '').length) return;
    let best = 0, bestDistance = HIT;
    for (const [handle, ant] of ants) {
      if (handle === you || ant.asleep || ant.leaving) continue;
      const d = Math.hypot(ant.body.x - event.clientX, ant.body.y - event.clientY);
      if (d < bestDistance) { best = handle; bestDistance = d; }
    }
    if (best) onTap(best);
  });

  function start() { if (!running) { running = true; last = 0; requestAnimationFrame(frame); } }
  function stop() { running = false; }
  document.addEventListener('visibilitychange', () => (document.hidden ? stop() : shown && start()));

  return {
    welcome, apply,
    batch(messages) { for (const message of messages) apply(message); },
    field(message) { field = message; },
    show() { shown = true; canvas.classList.add('ants--on'); if (!document.hidden) start(); },
    hide() { shown = false; canvas.classList.remove('ants--on'); setTimeout(() => { if (!shown) { stop(); ants.clear(); } }, 600); },
    /** Another ant, awake, for the menu's "Greet an ant". */
    someone() {
      const others = [...ants.entries()].filter(([h, ant]) => h !== you && !ant.asleep && !ant.leaving);
      return others.length ? others[Math.floor(Math.random() * others.length)][0] : 0;
    },
    count: () => ants.size,
  };
}

// The rig's parts as Path2D, each with the joints it hangs from, outermost
// first (a leg; the head, then an antenna), as rig.mjs poses the SVG.
function buildRig(svgText) {
  const svg = new DOMParser().parseFromString(svgText, 'image/svg+xml');
  const pivots = {};
  for (const el of svg.querySelectorAll('[data-pivot]')) pivots[el.id] = el.getAttribute('data-pivot').split(' ').map(Number);
  const JOINTS = new Set(['head', 'gaster', 'petiole', 'antenna-left', 'antenna-right', 'mandible-left', 'mandible-right',
    'leg-front-left', 'leg-front-right', 'leg-middle-left', 'leg-middle-right', 'leg-hind-left', 'leg-hind-right']);
  const parts = [];
  for (const path of svg.querySelectorAll('#ant path')) {
    const chain = [];
    for (let el = path; el && el.id !== 'ant'; el = el.parentElement) if (JOINTS.has(el.id)) chain.unshift(el.id);
    parts.push({ path: new Path2D(path.getAttribute('d')), fill: path.getAttribute('fill'), chain, gaster: path.id === 'gaster' });
  }
  return { parts, pivots };
}
