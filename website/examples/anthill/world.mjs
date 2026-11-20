// Two anthills: every ant and both queens are actors (ant.scxml,
// queen.scxml); the world is their I/O processor, urn:world. An ant decides
// only when something happens to it: it asks the world for a leg (forage,
// home, rest), and the world picks where it goes and how long it takes,
// records the leg, and tells the ant when it arrives (arrive, or found when
// it reached food). The world also tells two ants of different colonies
// that meet at the end of their legs to fight, and tells an old ant to die.
// Between decisions nothing runs: a renderer draws each ant on its leg,
// from the leg's two points and two times.
//
// The world takes the tinyactors package as an argument, so the same code
// runs in Node.js (main.mjs, with @tinyactors/wasm from npm) and in a
// browser Worker (worker.mjs, with the copy the website serves).

export const WIDTH = 1000;
export const HEIGHT = 600;
export const FRAME = 50; // simulated milliseconds per step
export const HILLS = [{ x: 210, y: 300 }, { x: 790, y: 300 }];
export const HILL_RADIUS = 26;

const SPEED = 40; // world units per simulated second
const LEG = { min: 2, max: 10 }; // simulated seconds a leg takes
const LIFE = { min: 120, max: 240 }; // a worker's life, simulated seconds
const MEETING = 0.5; // seconds two enemies' legs may end apart and still meet
const CELL = 8; // the meeting grid's cell, world units
const FOOD = { sources: 64, every: 10 }; // at most 64 sources; a new pair every 10 simulated seconds
const EVENTS = ['arrive', 'found', 'die'];
const ARRIVE = 0, FOUND = 1, DIE = 2;
const BROOD = 0, WORKER = 1;
const CHUNK = 4096; // ants spawned between runs to idle (a burst would hold a workspace per ant)

export async function createAnthill(tinyactors, { charts, population, seed = 1 }) {
  const { compileCharts, Host, SimulatedClock } = tinyactors;
  const CAP = population;
  const random = mulberry32(seed);
  const clock = new SimulatedClock();
  const now = () => clock.now / 1000;

  // ---- the ants' slots: where each one is, and what it is waiting for
  const x0 = new Float32Array(CAP), y0 = new Float32Array(CAP), x1 = new Float32Array(CAP), y1 = new Float32Array(CAP);
  const t0 = new Float32Array(CAP), t1 = new Float32Array(CAP), deathAt = new Float32Array(CAP);
  const colonyOf = new Uint8Array(CAP), stage = new Uint8Array(CAP), outside = new Uint8Array(CAP);
  const carrying = new Uint8Array(CAP), pending = new Uint8Array(CAP), target = new Int16Array(CAP);
  const sessionOf = new Float64Array(CAP); // 0: the slot is free
  let slotOf = new Int32Array(1024).fill(-1); // session → slot (sessions are never reused)
  const free = new Int32Array(CAP);
  let freeTop = 0;
  for (let slot = CAP - 1; slot >= 0; slot--) free[freeTop++] = slot;

  // ---- what changed since the renderer last asked
  const changed = new Int32Array(CAP);
  const marked = new Uint8Array(CAP);
  let changes = 0;
  const touch = (slot) => { if (!marked[slot]) { marked[slot] = 1; changed[changes++] = slot; } };

  // ---- arrivals, in buckets of one frame
  const RING = Math.ceil((LEG.max * 1000) / FRAME) + 8;
  const ring = Array.from({ length: RING }, () => []);
  const due = (slot, at, kind) => {
    pending[slot] = kind;
    ring[Math.floor((at * 1000) / FRAME) % RING].push(slot, sessionOf[slot]);
  };

  // ---- food, the meeting grid, the colonies
  const foodX = new Float32Array(FOOD.sources), foodY = new Float32Array(FOOD.sources), foodLeft = new Int32Array(FOOD.sources);
  let foods = 0;
  const COLS = Math.ceil(WIDTH / CELL), ROWS = Math.ceil(HEIGHT / CELL);
  const cellAt = new Float32Array(COLS * ROWS).fill(-1e9), cellSlot = new Int32Array(COLS * ROWS), cellColony = new Int8Array(COLS * ROWS).fill(-1);
  // A meeting turns into a fight one time in two, less often in a crowd:
  // meetings grow faster than the population, and so would the deaths.
  const FIGHT = 0.5 * Math.min(1, Math.sqrt(30_000 / CAP));
  const colonies = HILLS.map(() => ({ queen: 0, workers: 0, brood: 0, harvest: 0, stored: 0, fights: 0, deaths: 0, born: 0 }));
  let decisions = 0;

  // A source holds a tenth of the population's worth of food: food stays
  // what limits a colony, at every population.
  const AMOUNT = Math.max(200, Math.round(CAP / 10));
  function addFood(x, y, amount = AMOUNT) {
    const i = foods < FOOD.sources ? foods++ : foodLeft.indexOf(Math.min(...foodLeft.slice(0, foods)));
    foodX[i] = x; foodY[i] = y; foodLeft[i] = amount;
  }
  // Food appears in mirrored pairs, so neither colony is favoured and both
  // meet over it in the middle.
  function sprout() {
    const x = 330 + random() * 340, y = 60 + random() * 480;
    addFood(x, y); addFood(WIDTH - x, HEIGHT - y);
  }
  for (let i = 0; i < 4; i++) sprout();

  // ---- the world: what an ant asks for
  function leg(slot, x, y, kind, seconds) {
    const t = now();
    x0[slot] = x1[slot]; y0[slot] = y1[slot]; x1[slot] = x; y1[slot] = y;
    t0[slot] = t; t1[slot] = t + seconds;
    touch(slot);
    due(slot, t + seconds, kind);
  }
  const travel = (slot, x, y) => Math.min(LEG.max, Math.max(LEG.min, Math.hypot(x - x1[slot], y - y1[slot]) / SPEED));

  function forage(slot) {
    const colony = colonyOf[slot];
    if (stage[slot] === BROOD) { // a new worker leaves the hill
      stage[slot] = WORKER; colonies[colony].brood--; colonies[colony].workers++;
      deathAt[slot] = now() + LIFE.min + random() * (LIFE.max - LIFE.min);
      placeInHill(slot, colony);
    }
    if (now() >= deathAt[slot]) { due(slot, now() + FRAME / 1000, DIE); return; }
    outside[slot] = 1;
    // Most legs head for food the ant can see (within 260 units), the rest wander.
    let best = -1, bestDistance = 260;
    if (random() < 0.8) {
      for (let i = 0; i < foods; i++) {
        if (foodLeft[i] <= 0) continue;
        const d = Math.hypot(foodX[i] - x1[slot], foodY[i] - y1[slot]) * (0.6 + random() * 0.8);
        if (d < bestDistance) { best = i; bestDistance = d; }
      }
    }
    if (best >= 0) {
      target[slot] = best;
      const x = foodX[best] + (random() - 0.5) * 16, y = foodY[best] + (random() - 0.5) * 16;
      leg(slot, x, y, FOUND, travel(slot, x, y));
    } else {
      target[slot] = -1;
      // A wander drifts a fifth of the way back toward the middle of the
      // field, so ants do not pile up along its edges.
      const angle = random() * Math.PI * 2, distance = 40 + random() * 120;
      const x = clamp(x1[slot] + Math.cos(angle) * distance + (WIDTH / 2 - x1[slot]) * 0.2, 12, WIDTH - 12);
      const y = clamp(y1[slot] + Math.sin(angle) * distance + (HEIGHT / 2 - y1[slot]) * 0.2, 12, HEIGHT - 12);
      leg(slot, x, y, ARRIVE, travel(slot, x, y));
    }
  }

  function home(slot) {
    const hill = HILLS[colonyOf[slot]];
    carrying[slot] = 1;
    const x = hill.x + (random() - 0.5) * 10, y = hill.y + (random() - 0.5) * 10;
    leg(slot, x, y, ARRIVE, travel(slot, x, y));
  }

  function rest(slot) {
    outside[slot] = 0;
    const hill = HILLS[colonyOf[slot]];
    leg(slot, hill.x, hill.y, ARRIVE, 6 + random() * 14); // a rest in the hill: 6 to 20 seconds
  }

  function placeInHill(slot, colony) {
    const hill = HILLS[colony], angle = random() * Math.PI * 2;
    x1[slot] = hill.x + Math.cos(angle) * HILL_RADIUS * 0.8;
    y1[slot] = hill.y + Math.sin(angle) * HILL_RADIUS * 0.8;
  }

  let host;
  function lay(colony, eggs) {
    const room = Math.min(eggs, freeTop, CAP - alive());
    for (let i = 0; i < room; i++) hatch(colony, BROOD);
  }

  function hatch(colony, kind) {
    const actor = host.spawn('ant', { params: { colony, stage: kind === BROOD ? 'egg' : 'worker' } });
    const session = Number(actor.id);
    const slot = free[--freeTop];
    if (session >= slotOf.length) {
      const grown = new Int32Array(Math.max(session + 1, slotOf.length * 2)).fill(-1);
      grown.set(slotOf);
      slotOf = grown;
    }
    slotOf[session] = slot;
    sessionOf[slot] = session;
    colonyOf[slot] = colony; stage[slot] = BROOD; outside[slot] = 0; carrying[slot] = 0;
    placeInHill(slot, colony);
    colonies[colony].brood++; colonies[colony].born++;
    touch(slot);
  }

  const alive = () => colonies.reduce((sum, colony) => sum + colony.workers + colony.brood, 0);

  const world = {
    receive(send) {
      decisions++;
      const session = Number(send.session);
      if (send.event === 'lay') {
        const colony = queens.indexOf(session);
        if (colony >= 0) lay(colony, send.data.eggs);
        return;
      }
      const slot = slotOf[session];
      if (slot < 0 || sessionOf[slot] !== session) return;
      if (send.event === 'forage') forage(slot);
      else if (send.event === 'home') home(slot);
      else if (send.event === 'rest') rest(slot);
    },
  };

  const compiled = compileCharts([
    { name: 'ant.scxml', text: charts.ant },
    { name: 'queen.scxml', text: charts.queen },
  ]);
  host = new Host(compiled, {
    system: 'anthill',
    clock,
    config: { system: { maxActors: CAP + 64, maxEvents: Math.max(256, Math.ceil(CAP / 4)), maxTimers: Math.max(64, Math.ceil(CAP / 8)), tracing: false } },
    ioProcessors: { 'urn:world': world },
    onDone: (actor) => {
      const session = Number(actor.id);
      const slot = slotOf[session];
      if (slot < 0 || sessionOf[slot] !== session) return;
      const colony = colonies[colonyOf[slot]];
      if (stage[slot] === BROOD) colony.brood--; else colony.workers--;
      colony.deaths++;
      sessionOf[slot] = 0; slotOf[session] = -1; outside[slot] = 0;
      free[freeTop++] = slot;
      touch(slot);
    },
  });
  compiled.close();
  const queens = HILLS.map((_, colony) => Number(host.spawn('queen', { params: { colony } }).id));
  colonies.forEach((colony, i) => { colony.queen = queens[i]; });

  /** Spawns the first population: nine in ten workers, the rest brood. */
  async function populate(progress = () => {}) {
    for (let start = 0; start < CAP; start += CHUNK) {
      for (let i = start; i < Math.min(CAP, start + CHUNK); i++) hatch(i & 1, random() < 0.9 ? WORKER : BROOD);
      await host.idle();
      progress(Math.min(CAP, start + CHUNK) / CAP);
    }
  }

  /** One frame: the clock moves on, the ants whose legs end hear of it. */
  async function step() {
    clock.increment(FRAME);
    const t = now();
    const bucket = ring[Math.floor(clock.now / FRAME) % RING];
    ring[Math.floor(clock.now / FRAME) % RING] = [];
    const items = [];
    for (let i = 0; i < bucket.length; i += 2) {
      const slot = bucket[i], session = bucket[i + 1];
      if (sessionOf[slot] !== session) continue; // ended, or a newer ant in the slot
      let kind = pending[slot];
      if (kind === FOUND) {
        const food = target[slot];
        if (food >= 0 && foodLeft[food] > 0) foodLeft[food]--; else kind = ARRIVE;
      }
      if (kind === ARRIVE && carrying[slot]) { carrying[slot] = 0; colonies[colonyOf[slot]].harvest++; colonies[colonyOf[slot]].stored++; }
      items.push({ target: session, event: EVENTS[kind] });
      if (outside[slot] && kind !== DIE) meet(slot, t, items);
    }
    host.sendMany(items);
    if (clock.now % (FOOD.every * 1000) === 0) sprout();
    if (clock.now % 1000 === 0) {
      colonies.forEach((colony, i) => {
        if (colony.harvest > 0) host.send(queens[i], 'harvest', { food: colony.harvest });
        colony.harvest = 0;
      });
    }
    await host.idle();
  }

  // Two ants of different colonies whose legs end in one cell within half
  // a second of each other meet, and may fight.
  function meet(slot, t, items) {
    const cell = Math.floor(y1[slot] / CELL) * COLS + Math.floor(x1[slot] / CELL);
    const other = cellSlot[cell];
    if (cellColony[cell] >= 0 && cellColony[cell] !== colonyOf[slot] && t - cellAt[cell] < MEETING && sessionOf[other] && outside[other] && random() < FIGHT) {
      items.push({ target: sessionOf[slot], event: 'fight' }, { target: sessionOf[other], event: 'fight' });
      colonies[0].fights++;
      cellColony[cell] = -1;
      return;
    }
    cellAt[cell] = t; cellSlot[cell] = slot; cellColony[cell] = colonyOf[slot];
  }

  /** Kills the ants outside within `radius` of (x, y), now; answers how many. */
  function squash(x, y, radius) {
    const t = now();
    let killed = 0;
    for (let slot = 0; slot < CAP; slot++) {
      if (!sessionOf[slot] || !outside[slot]) continue;
      const [px, py] = position(slot, t);
      if ((px - x) ** 2 + (py - y) ** 2 > radius * radius) continue;
      host.send(sessionOf[slot], 'kill');
      killed++;
    }
    return killed;
  }

  function position(slot, t) {
    const span = t1[slot] - t0[slot];
    const f = span > 0 ? Math.min(1, Math.max(0, (t - t0[slot]) / span)) : 1;
    return [x0[slot] + (x1[slot] - x0[slot]) * f, y0[slot] + (y1[slot] - y0[slot]) * f];
  }

  /** The legs that changed since the last call: [slot, x0, y0, x1, y1, t0, t1, look] each, look 0 hidden, 1 or 2 its colony. */
  function delta() {
    const out = new Float32Array(changes * 8);
    for (let i = 0; i < changes; i++) {
      const slot = changed[i];
      marked[slot] = 0;
      const at = i * 8;
      out[at] = slot; out[at + 1] = x0[slot]; out[at + 2] = y0[slot]; out[at + 3] = x1[slot]; out[at + 4] = y1[slot];
      out[at + 5] = t0[slot]; out[at + 6] = t1[slot]; out[at + 7] = sessionOf[slot] && outside[slot] ? colonyOf[slot] + 1 : 0;
    }
    changes = 0;
    return out;
  }

  function stats() {
    return {
      time: now(),
      decisions,
      colonies: colonies.map(({ workers, brood, stored, deaths, born }) => ({ workers, brood, stored, deaths, born })),
      fights: colonies[0].fights,
      food: Array.from({ length: foods }, (_, i) => [foodX[i], foodY[i], foodLeft[i]]),
    };
  }

  return { populate, step, squash, addFood, delta, stats, close: () => host.close(), capacity: CAP };
}

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
