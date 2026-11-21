// An ant's body in motion: a point with a heading, moved by a forward speed
// and a turn rate, and the joint angles that motion implies. Pure: no DOM, so
// the same code runs in the page and in Node (to plan a scene's path).
//
// Heading is degrees clockwise from up (the SVG ant faces up), so 90 faces
// right. Speeds are px/s and deg/s on the stage.
//
// The legs follow measured ant gait (Wahl, Pfeffer & Wittlinger 2015,
// Cataglyphis fortis, high-speed video): an alternating tripod; a foot stays
// planted on the ground for 60% of the cycle (duty factor about 0.6 at
// walking speeds); stride length grows with speed, about 0.4 body lengths at
// a slow walk and 1 body length at 10 body lengths a second. Each foot is
// planted in world space during stance and the leg is solved to reach it, so
// nothing slides; a swinging foot is placed ahead of its rest point by half
// the coming stance's travel, which also makes turns right: the outer legs
// take long steps, the inner short ones, and turning in place they step
// against each other.

// The rig's legs in SVG units: the joint (data-pivot) and the foot at rest.
export const LEGS = {
  'leg-front-left':   { pivot: [445.9, 447.0], foot: [263.5, 260.6], group: 0 },
  'leg-front-right':  { pivot: [578.3, 448.1], foot: [759.1, 260.7], group: 1 },
  'leg-middle-left':  { pivot: [455.7, 490.1], foot: [195.5, 614.2], group: 1 },
  'leg-middle-right': { pivot: [568.5, 489.7], foot: [828.6, 614.2], group: 0 },
  'leg-hind-left':    { pivot: [475.2, 563.6], foot: [289.4, 866.0], group: 0 },
  'leg-hind-right':   { pivot: [548.8, 563.5], foot: [734.4, 866.0], group: 1 },
};
export const CENTER = [512, 530];      // the point the ant turns about, SVG units
const BODY = 633;                      // mandibles to gaster tip, SVG units
const DUTY = 0.6;                      // the part of a cycle a foot is down
const TURN_RADIUS = 290;               // a foot's distance from the centre, SVG units
const TUCK = { front: -34, middle: -14, hind: 22 }; // legs folded along the body, degrees

const approach = (x, target, rate, dt) => x + (target - x) * (1 - Math.exp(-rate * dt));
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const rad = (d) => (d * Math.PI) / 180;
const frac = (x) => ((x % 1) + 1) % 1;

/** Stride length in body lengths at a speed in body lengths a second (fit to Wahl et al.). */
export function strideBL(speedBL) {
  return clamp(0.4 + 0.06 * speedBL, 0.4, 2);
}

function toWorld(a, [lx, ly], heading = a.heading + a.sway, x = a.x, y = a.y) {
  const h = rad(heading), c = Math.cos(h), s = Math.sin(h);
  const dx = (lx - CENTER[0]) * a.scale, dy = (ly - CENTER[1]) * a.scale;
  return [x + c * dx - s * dy, y + s * dx + c * dy];
}
function toLocal(a, [wx, wy]) {
  const h = rad(-(a.heading + a.sway)), c = Math.cos(h), s = Math.sin(h);
  const dx = (wx - a.x) / a.scale, dy = (wy - a.y) / a.scale;
  return [CENTER[0] + c * dx - s * dy, CENTER[1] + s * dx + c * dy];
}

/** Where a leg's rest point will be in `tau` seconds, at the current speed and turn rate. */
function restAhead(a, leg, tau) {
  let x = a.x, y = a.y, h = a.heading;
  const n = 8, dt = tau / n;
  for (let i = 0; i < n; i++) {
    const m = rad(h + (a.w * dt) / 2);
    x += a.v * Math.sin(m) * dt;
    y -= a.v * Math.cos(m) * dt;
    h += a.w * dt;
  }
  return toWorld(a, leg.foot, h, x, y);
}

export function makeAnt(init) {
  const a = {
    x: 0, y: 0, heading: 0, v: 0, w: 0, scale: 0.21,
    target: { v: 0, w: 0, tuck: 0, greet: 0, idle: 0, eat: 0, alpha: 1 },
    phase: 0, rate: 0, sway: 0, headYaw: 0, gasterYaw: 0, tuck: 0, greet: 0, idle: 0, eat: 0, alpha: 1,
    flick: 0, odometer: 0, trail: [], time: 0, seed: 0,
    ...init,
  };
  a.feet = {};
  for (const [id, leg] of Object.entries(LEGS)) {
    a.feet[id] = { at: toWorld(a, leg.foot), from: null, down: true };
  }
  return a;
}

/** Advance one ant by dt seconds. */
export function advance(a, dt) {
  a.time += dt;
  const wasAsleep = a.target.tuck;
  a.v = approach(a.v, a.target.v, 5, dt);
  a.w = approach(a.w, a.target.w, 6, dt);
  const h = rad(a.heading);
  a.x += a.v * Math.sin(h) * dt;
  a.y -= a.v * Math.cos(h) * dt;
  a.heading += a.w * dt;

  // gait rate from the speed the feet see: forward travel plus the arc of turning
  const bodyPx = BODY * a.scale;
  const footSpeed = Math.abs(a.v) + Math.abs(rad(a.w)) * TURN_RADIUS * a.scale;
  const speedBL = footSpeed / bodyPx;
  const want = footSpeed > 1 ? footSpeed / (strideBL(speedBL) * bodyPx) : 0;
  a.rate = want;
  // at a standstill, finish the step in progress: run on until all six feet are down
  const p = frac(a.phase);
  if (want < 0.3 && a.tuck < 0.5) {
    const settled = (p >= 0.5 && p < 0.6) || p < 0.1;
    a.rate = settled ? want : Math.max(want, 2.2);
  }
  if (a.tuck > 0.05) a.rate = 0; // folding up: feet stay where they are
  a.phase += a.rate * dt;
  a.gait = clamp(want / 2.5, 0, 1);
  // Eating rocks the body a little as the head works.
  a.sway = 1.2 * a.gait * Math.sin(2 * Math.PI * a.phase) + a.eat * 1.4 * Math.sin(2 * Math.PI * 1.8 * a.time);

  // the feet
  a.legs = {};
  for (const [id, leg] of Object.entries(LEGS)) {
    const foot = a.feet[id];
    const q = frac(a.phase + leg.group * 0.5); // 0..DUTY stance, DUTY..1 swing
    const swinging = q >= DUTY && a.rate > 0;
    if (swinging) {
      if (foot.down) { foot.down = false; foot.from = foot.at; }
      const s = (q - DUTY) / (1 - DUTY);            // swing progress 0..1
      const left = ((1 - s) * (1 - DUTY)) / Math.max(a.rate, 1e-6); // seconds to touchdown
      const stance = DUTY / Math.max(a.rate, 1e-6);
      const target = restAhead(a, leg, left + (want > 0.3 ? stance / 2 : 0));
      const e = s * s * (3 - 2 * s);
      foot.at = [foot.from[0] + (target[0] - foot.from[0]) * e, foot.from[1] + (target[1] - foot.from[1]) * e];
      foot.lift = Math.sin(Math.PI * s);
    } else {
      foot.down = true;
      foot.lift = 0;
    }
    // solve the leg to reach its foot
    const [px, py] = leg.pivot;
    const [fx, fy] = toLocal(a, foot.at);
    const restAngle = Math.atan2(leg.foot[1] - py, leg.foot[0] - px);
    const restLen = Math.hypot(leg.foot[0] - px, leg.foot[1] - py);
    let angle = ((Math.atan2(fy - py, fx - px) - restAngle) * 180) / Math.PI;
    angle = ((angle + 540) % 360) - 180;
    let reach = clamp(Math.hypot(fx - px, fy - py) / restLen, 0.72, 1.12);
    // folding: blend toward the tucked pose
    const pair = id.split('-')[1];
    const outward = id.endsWith('left') ? 1 : -1;
    angle = angle * (1 - a.tuck) + outward * TUCK[pair] * a.tuck;
    reach = reach * (1 - a.tuck) + 0.7 * a.tuck;
    a.legs[id] = { angle, scale: reach * (1 - 0.06 * foot.lift) };
  }

  const prevTuck = a.tuck;
  a.tuck = approach(a.tuck, a.target.tuck, 2.2, dt);
  if (prevTuck > 0.5 && a.tuck <= 0.5 && wasAsleep === 0) a.flick = 1; // waking: the antennae flick
  if (prevTuck > 0.05 && a.tuck <= 0.05) for (const id in LEGS) a.feet[id].at = toWorld(a, LEGS[id].foot); // unfolded: feet back under the body
  a.flick = approach(a.flick, 0, 2.5, dt);
  a.greet = approach(a.greet, a.target.greet, 4, dt);
  a.eat = approach(a.eat, a.target.eat, 5, dt);
  a.idle = approach(a.idle, a.target.idle, 2, dt);
  a.alpha = approach(a.alpha, a.target.alpha, 3, dt);

  a.headYaw = approach(a.headYaw, clamp(a.w * 0.16, -18, 18) + a.idle * 9 * Math.sin(a.time * 0.9 + a.seed), 9, dt);
  a.gasterYaw = approach(a.gasterYaw, clamp(-a.w * 0.1, -12, 12), 2.5, dt);

  a.odometer += Math.abs(a.v) * dt;
  if (a.odometer > 13 && a.alpha > 0.5) {
    a.odometer = 0;
    a.trail.push({ x: a.x, y: a.y, t: a.time });
    if (a.trail.length > 400) a.trail.shift();
  }

  const cyc = 2 * Math.PI * a.phase;
  const sweep = 7 * a.gait * Math.sin(cyc * 0.5);
  const lean = clamp(a.w * 0.12, -14, 14);
  const t = a.time;
  const explore = a.idle * 15;
  const tap = a.greet * (16 + 9 * Math.sin(2 * Math.PI * 4.5 * t));
  const tapR = a.greet * (16 + 9 * Math.sin(2 * Math.PI * 4.5 * t + 1.9));
  const flick = a.flick * 22 * Math.sin(2 * Math.PI * 3 * t);
  // Eating: the antennae drum forward on the food, the head nods and dips
  // to it, the mandibles chew.
  const drum = a.eat * (14 + 7 * Math.sin(2 * Math.PI * 2.4 * t));
  const drumR = a.eat * (14 + 7 * Math.sin(2 * Math.PI * 2.4 * t + 2.2));
  const nod = Math.sin(2 * Math.PI * 1.8 * t);
  a.pose = {
    legs: a.legs, tuck: a.tuck,
    headYaw: a.headYaw + a.sway * 0.6 + a.eat * 4 * nod,
    headDip: a.eat * (0.035 + 0.025 * nod),
    gasterYaw: a.gasterYaw - a.sway * 1.4,
    petioleYaw: a.gasterYaw * 0.4 - a.sway * 0.6,
    antL: sweep + lean + explore * Math.sin(t * 2.3 + a.seed) + tap + flick + drum,
    antR: -sweep - lean + explore * Math.sin(t * 1.9 + a.seed + 2) + tapR + flick + drumR,
    mandible: Math.min(1, a.greet * (0.5 + 0.5 * Math.sin(2 * Math.PI * 2 * t)) + a.idle * 0.25 * Math.max(0, Math.sin(t * 1.3 + a.seed))
      + a.eat * (0.55 + 0.45 * Math.sin(2 * Math.PI * 3.5 * t))),
  };
}
