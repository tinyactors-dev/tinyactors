// The motion study: two ants on a 1280x720 stage, one cue list. Each cue sets
// an ant's targets at a time; the body eases toward them.
import { makeAnt, advance } from './ant.mjs';

export const W = 1280, H = 720, DURATION = 31;

export const CHAPTERS = [
  [0, '01', 'Walk', 'Tripod gait: three legs down, three swinging. Steps match the ground speed.'],
  [3.6, '02', 'Turn while walking', 'The head leads, the gaster lags, the outer legs take the longer strides.'],
  [7.6, '03', 'Turn in place', 'The two sides stroke against each other: left forward, right back.'],
  [10.2, '04', 'Idle', 'At rest the antennae explore and the head looks around.'],
  [12.6, '05', 'Arrive', 'Another visitor opens the page: a new actor walks in.'],
  [16.8, '06', 'Greet', 'Antennation: the two ants tap antennae, mandibles working.'],
  [19.6, '07', 'Hibernate', 'The tab is hidden: legs and antennae fold, the colour fades.'],
  [23.2, '08', 'Wake', 'The tab is back: it unfolds and the antennae flick.'],
  [25.6, '09', 'Leave', 'The tab is closed: the actor reaches its final state and walks away.'],
];

const CUES = [
  [0, 'a', { v: 125, w: 0 }],
  [3.6, 'a', { v: 115, w: -34 }],
  [5.6, 'a', { v: 115, w: 34 }],
  [7.6, 'a', { v: 0, w: 0 }],
  [7.9, 'a', { v: 0, w: 95 }],
  [9.8, 'a', { v: 0, w: 0 }],
  [10.2, 'a', { idle: 1 }],
  [12.6, 'b', { v: 150, w: 0, alpha: 1 }],
  [12.6, 'a', { idle: 0.4 }],
  [16.05, 'b', { v: 0 }],
  [16.8, 'a', { idle: 0, greet: 1 }],
  [16.8, 'b', { greet: 1 }],
  [19.2, 'a', { greet: 0 }],
  [19.2, 'b', { greet: 0, idle: 0.6 }],
  [19.6, 'a', { tuck: 1, alpha: 0.42, status: 'hibernated' }],
  [23.2, 'a', { tuck: 0, alpha: 1, status: 'running' }],
  [24.8, 'a', { idle: 0.5 }],
  [25.6, 'b', { idle: 0, w: -110, status: 'done' }],
  [27.2, 'b', { w: 0, v: 135 }],
  [27.6, 'a', { idle: 0, w: 70, v: 60, status: 'done' }],
  [28.6, 'a', { w: 0, v: 135 }],
];

export function makeScene() {
  const ants = {
    a: makeAnt({ x: -170, y: 580, heading: 90, v: 125, seed: 1, trailColor: '#1F5E4A', name: '7f3a' }),
    b: makeAnt({ x: -70, y: 338, heading: 90, v: 150, seed: 4, alpha: 1, trailColor: '#D8A64E', name: 'c21e' }),
  };
  ants.a.target.v = 125;
  ants.b.hidden = true;
  for (const ant of Object.values(ants)) advance(ant, 0);
  let t = 0, cue = 0;
  const DT = 1 / 240;
  return {
    ants,
    get time() { return t; },
    advanceTo(until) {
      while (t < until - 1e-9) {
        while (cue < CUES.length && CUES[cue][0] <= t) {
          const [, id, set] = CUES[cue++];
          Object.assign(ants[id].target, set);
          if (id === 'b') ants.b.hidden = false;
        }
        for (const ant of Object.values(ants)) if (!ant.hidden) advance(ant, DT);
        t += DT;
      }
    },
    chapter(at) {
      let c = CHAPTERS[0];
      for (const ch of CHAPTERS) if (ch[0] <= at) c = ch;
      return c;
    },
  };
}
