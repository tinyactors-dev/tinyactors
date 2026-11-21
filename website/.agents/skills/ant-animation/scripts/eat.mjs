// The eating study: an ant smells a pile of food, walks to it, takes three
// bites (the pile shrinks with each, and is gone after the last), then walks
// on. It steers after a point as the presence layer does (website/presence/
// layer.mjs): turn toward it, walk when roughly facing it, stop just short.
// node render.mjs video 60 <outdir> eat
import { makeAnt, advance } from './ant.mjs';

export const W = 1280, H = 720, DURATION = 16;

export const CHAPTERS = [
  [0, '01', 'Smell food', 'The antennae lead; the ant walks to the pile and stops facing it.'],
  [4.6, '02', 'Eat', 'Three bites: antennae drum on the food, the head nods, the mandibles chew.'],
  [11.4, '03', 'Finished', 'The last bite takes the pile; the ant carries its crumb away.'],
];

const PILE = { x: 760, y: 400 };
const BITES = [5.2, 7.2, 9.2]; // when each bite comes off the pile
const STOP = 92;               // the ant's centre this far from the pile: its mandibles at the seeds' edge

export function makeScene() {
  const ant = makeAnt({ x: 120, y: 470, heading: 75, seed: 2, trailColor: '#1F5E4A', name: '7f3a' });
  const ants = { a: ant };
  const scene = { food: [{ ...PILE, left: 1 }] };
  let t = 0;
  const DT = 1 / 240;
  advance(ant, 0);

  function steer(tx, ty, dt) {
    const dx = tx - ant.x, dy = ty - ant.y, distance = Math.hypot(dx, dy);
    const facing = (Math.atan2(PILE.x - ant.x, -(PILE.y - ant.y)) * 180) / Math.PI;
    const wanted = distance > 2 ? (Math.atan2(dx, -dy) * 180) / Math.PI : facing;
    const error = ((wanted - ant.heading + 540) % 360) - 180;
    ant.target.w = Math.max(-140, Math.min(140, error * 3));
    ant.target.v = distance > 2 ? Math.min(125, distance * 1.8) * Math.max(0, Math.cos((error * Math.PI) / 180)) : 0;
    advance(ant, dt);
  }

  Object.defineProperty(scene, 'time', { get: () => t }); // a getter: Object.assign would copy its value once
  Object.assign(scene, {
    ants,
    advanceTo(until) {
      while (t < until - 1e-9) {
        const pile = scene.food[0];
        if (t < 11.4) {
          const angle = Math.atan2(PILE.y - 470, PILE.x - 120);
          steer(PILE.x - Math.cos(angle) * STOP, PILE.y - Math.sin(angle) * STOP, DT);
          ant.target.eat = t > 4.6 && t < 11 ? 1 : 0;
        } else {
          steer(1500, 180, DT); // on, with its crumb
          ant.target.eat = 0;
        }
        for (const at of BITES) {
          if (pile && t < at && t + DT >= at) { pile.left = Math.max(0, pile.left - 1 / BITES.length); ant.carry = true; }
        }
        if (pile && pile.left < 0.01) scene.food = [];
        t += DT;
      }
    },
    chapter(at) {
      let c = CHAPTERS[0];
      for (const ch of CHAPTERS) if (ch[0] <= at) c = ch;
      return c;
    },
  });
  return scene;
}
