// The design system's rigged ant (ant-resting.svg), posed by joint angles.
// Every part rotates about its own data-pivot. Angles are degrees; for legs
// and antennae a positive angle swings the part forward (toward the head) on
// either side, so callers never think about left and right mirroring.

const LEGS = ['front', 'middle', 'hind'];
const TRIPOD_A = new Set(['leg-front-left', 'leg-middle-right', 'leg-hind-left']);

function pivot(el) {
  const p = el.getAttribute('data-pivot');
  return p ? p.split(' ').map(Number) : null;
}

/** Make a rig over a cloned #ant group. */
export function rig(antGroup) {
  const q = (id) => antGroup.querySelector(`[id="${id}"]`);
  const part = (id, fallbackPivot) => {
    const el = q(id);
    el.removeAttribute('style'); // the CSS transform-origin would shift the attribute transform
    el.removeAttribute('id'); // several ants share a document
    return { el, at: pivot(el) ?? fallbackPivot };
  };
  const legs = {};
  for (const pair of LEGS) for (const side of ['left', 'right']) {
    const id = `leg-${pair}-${side}`;
    legs[id] = { ...part(id), outward: side === 'left' ? 1 : -1, tripod: TRIPOD_A.has(id) ? 0 : 0.5, pair };
  }
  const r = {
    legs,
    head: part('head'),
    gaster: part('gaster'),
    petiole: part('petiole'),
    antennaL: { ...part('antenna-left'), outward: 1 },
    antennaR: { ...part('antenna-right'), outward: -1 },
    mandibleL: { ...part('mandible-left'), outward: 1 },
    mandibleR: { ...part('mandible-right'), outward: -1 },
  };
  for (const el of antGroup.querySelectorAll('[id]')) el.removeAttribute('id');
  return r;
}

const rot = (a, [x, y]) => `rotate(${a.toFixed(3)} ${x} ${y})`;
const rotScale = (a, s, [x, y]) =>
  `rotate(${a.toFixed(3)} ${x} ${y}) translate(${x} ${y}) scale(${s.toFixed(4)}) translate(${-x} ${-y})`;

/**
 * Pose the rig.
 *  legs       per leg id, {angle, scale}: the angle the gait solved (SVG degrees) and the reach
 *  headYaw, gasterYaw, petioleYaw  degrees
 *  headDip    0..~0.06: the head lowered (eating), drawn smaller from above
 *  antL, antR antenna angles (forward positive), mandible 0..1 open, tuck 0..1
 */
export function pose(r, s) {
  for (const [id, leg] of Object.entries(r.legs)) {
    const { angle, scale } = s.legs[id]; // solved by the gait: SVG degrees, reach
    leg.el.setAttribute('transform', rotScale(angle, scale, leg.at));
  }
  r.head.el.setAttribute('transform', rotScale(s.headYaw, 1 - 0.03 * s.tuck - (s.headDip ?? 0), r.head.at)); // a dip: the head lowered, so it looks smaller from above
  r.gaster.el.setAttribute('transform', rot(s.gasterYaw, r.gaster.at));
  r.petiole.el.setAttribute('transform', rot(s.petioleYaw, r.petiole.at));
  const fold = -38 * s.tuck; // antennae swept back along the head
  r.antennaL.el.setAttribute('transform', rot(r.antennaL.outward * (s.antL + fold), r.antennaL.at));
  r.antennaR.el.setAttribute('transform', rot(r.antennaR.outward * (s.antR + fold), r.antennaR.at));
  const m = 9 * s.mandible;
  r.mandibleL.el.setAttribute('transform', rot(-r.mandibleL.outward * m, r.mandibleL.at));
  r.mandibleR.el.setAttribute('transform', rot(-r.mandibleR.outward * m, r.mandibleR.at));
}
