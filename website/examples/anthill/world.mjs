// One anthill: its ants and its queen are actors (ant.scxml, queen.scxml)
// on a host of their own, and the world is their I/O processor, urn:world.
// An ant decides only when something happens to it: it asks the world for
// a trip (forage, home, rest), and the world picks where it goes and how
// long it takes, records the trip, and tells the ant when it arrives
// (arrive, or found when it reached food). It also tells an old ant to die.
// Between decisions nothing runs: a renderer draws each ant on its trip,
// from the trip's two points and two times. A worker rests in the hill
// between outings, as long as the page's rest slider says: the longer the
// rests, the fewer ants are out and the fewer messages a second.
//
// The two colonies run side by side, a host each, and share the field:
// after every frame a colony hands the other a message of what it did
// (where its ants arrived, which enemies they struck, the food they ate)
// and takes the other's message before its next frame. So two hosts run in
// two Workers, and the field stays one: an ant that arrives where an enemy
// arrived less than half a second before may fight it.
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
const TRIP = { min: 2, max: 10 }; // simulated seconds a trip takes
const LIFE = { min: 6, max: 14 }; // a worker's life, in trips out of the hill
const MEETING = 0.5; // seconds apart two enemies may arrive at one spot and still meet
const CELL = 8; // the meeting grid's cell, world units
const FOOD = { sources: 64, every: 10 }; // at most 64 sources; a new pair every 10 simulated seconds
const EVENTS = ['arrive', 'found', 'die'];
const ARRIVE = 0, FOUND = 1, DIE = 2;
const BROOD = 0, WORKER = 1;
const CHUNK = 4096; // ants spawned between runs to idle (a burst would hold a workspace per ant)

/**
 * Colony `colony` of a field of two, with room for `population` ants. Both
 * colonies of a field take the same `seed`, so they place the same food.
 * Its slots are numbered from `offset` in what delta() answers.
 */
export async function createColony(tinyactors, { charts, population, colony, seed = 1, offset = 0, rest = 60 }) {
  const { compileCharts, Host, SimulatedClock } = tinyactors;
  const CAP = population;
  const random = mulberry32(seed * 2 + colony + 1);
  const foodRandom = mulberry32(seed); // the same in both colonies
  const clock = new SimulatedClock();
  const now = () => clock.now / 1000;
  const hill = HILLS[colony];

  // ---- the ants' slots: where each one is, and what it is waiting for
  const x0 = new Float32Array(CAP), y0 = new Float32Array(CAP), x1 = new Float32Array(CAP), y1 = new Float32Array(CAP);
  const t0 = new Float32Array(CAP), t1 = new Float32Array(CAP), tripsLeft = new Uint8Array(CAP);
  const stage = new Uint8Array(CAP), outside = new Uint8Array(CAP);
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

  // ---- arrivals, in buckets of one frame, the earliest the next frame
  const buckets = new Map(); // frame -> [slot, session, ...]
  const due = (slot, at, kind) => {
    pending[slot] = kind;
    const frame = Math.max(Math.floor((at * 1000) / FRAME), clock.now / FRAME + 1);
    let bucket = buckets.get(frame);
    if (!bucket) buckets.set(frame, (bucket = []));
    bucket.push(slot, sessionOf[slot]);
  };

  // ---- food: both colonies hold the same sources, and each tells the other what it ate
  const foodX = new Float32Array(FOOD.sources), foodY = new Float32Array(FOOD.sources), foodLeft = new Int32Array(FOOD.sources);
  let eaten = new Int32Array(FOOD.sources);
  let foods = 0, replaced = 0;
  let placed = []; // food a reader placed here, for both colonies to add next frame
  const AMOUNT = Math.max(200, Math.round(CAP / 5)); // a tenth of the field's population
  function addFood(x, y) {
    const i = foods < FOOD.sources ? foods++ : replaced++ % FOOD.sources; // the oldest goes
    foodX[i] = x; foodY[i] = y; foodLeft[i] = AMOUNT;
  }
  // Food appears in mirrored pairs, so neither colony is favoured and both
  // meet over it in the middle.
  function sprout() {
    const x = 330 + foodRandom() * 340, y = 60 + foodRandom() * 480;
    addFood(x, y); addFood(WIDTH - x, HEIGHT - y);
  }
  for (let i = 0; i < 4; i++) sprout();

  // ---- enemies: where the other colony's ants arrived, a cell each
  const COLS = Math.ceil(WIDTH / CELL), ROWS = Math.ceil(HEIGHT / CELL);
  const enemyAt = new Float32Array(COLS * ROWS).fill(-1e9), enemySlot = new Int32Array(COLS * ROWS), enemySession = new Float64Array(COLS * ROWS);
  // A meeting turns into a fight one time in two, less often in a crowd:
  // meetings grow faster than the population, and so would the deaths.
  const FIGHT = 0.5 * Math.min(1, Math.sqrt(30_000 / (2 * CAP)));
  const counts = { workers: 0, brood: 0, out: 0, harvest: 0, stored: 0, fights: 0, deaths: 0, born: 0 };
  let decisions = 0, messages = 0; // decisions: sends to the world; messages: those and the events sent in
  let resting = rest; // the mean rest in the hill, simulated seconds
  const go = (slot, out) => { counts.out += out - outside[slot]; outside[slot] = out; };

  // ---- the world: what an ant asks for
  function trip(slot, x, y, kind, seconds) {
    const t = now();
    x0[slot] = x1[slot]; y0[slot] = y1[slot]; x1[slot] = x; y1[slot] = y;
    t0[slot] = t; t1[slot] = t + seconds;
    touch(slot);
    due(slot, t + seconds, kind);
  }
  const travel = (slot, x, y) => Math.min(TRIP.max, Math.max(TRIP.min, Math.hypot(x - x1[slot], y - y1[slot]) / SPEED));

  function forage(slot) {
    if (stage[slot] === BROOD) { // a new worker rests before its first trip, so workers leave at different times
      stage[slot] = WORKER; counts.brood--; counts.workers++;
      tripsLeft[slot] = LIFE.min + Math.floor(random() * (LIFE.max - LIFE.min + 1));
      placeInHill(slot);
      trip(slot, x1[slot], y1[slot], ARRIVE, random() * resting);
      return;
    }
    if (tripsLeft[slot]-- === 0) { due(slot, now() + FRAME / 1000, DIE); return; }
    go(slot, 1);
    // Most trips head for food the ant can see (within 260 units), the rest wander.
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
      trip(slot, x, y, FOUND, travel(slot, x, y));
    } else {
      target[slot] = -1;
      // A wander drifts a fifth of the way back toward the middle of the
      // field, so ants do not pile up along its edges.
      const angle = random() * Math.PI * 2, distance = 40 + random() * 120;
      const x = clamp(x1[slot] + Math.cos(angle) * distance + (WIDTH / 2 - x1[slot]) * 0.2, 12, WIDTH - 12);
      const y = clamp(y1[slot] + Math.sin(angle) * distance + (HEIGHT / 2 - y1[slot]) * 0.2, 12, HEIGHT - 12);
      trip(slot, x, y, ARRIVE, travel(slot, x, y));
    }
  }

  function home(slot) {
    carrying[slot] = 1;
    const x = hill.x + (random() - 0.5) * 10, y = hill.y + (random() - 0.5) * 10;
    trip(slot, x, y, ARRIVE, travel(slot, x, y));
  }

  function restInHill(slot) {
    go(slot, 0);
    trip(slot, hill.x, hill.y, ARRIVE, resting * (0.5 + random())); // half to one and a half times the mean
  }

  function placeInHill(slot) {
    const angle = random() * Math.PI * 2;
    x1[slot] = hill.x + Math.cos(angle) * HILL_RADIUS * 0.8;
    y1[slot] = hill.y + Math.sin(angle) * HILL_RADIUS * 0.8;
  }

  let host, queen = 0;
  function lay(eggs) {
    const room = Math.min(eggs, freeTop, CAP - counts.workers - counts.brood);
    for (let i = 0; i < room; i++) hatch(BROOD);
  }

  function hatch(kind) {
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
    stage[slot] = BROOD; carrying[slot] = 0;
    placeInHill(slot);
    counts.brood++; counts.born++;
    touch(slot);
  }

  const world = {
    receive(send) {
      decisions++; messages++;
      const session = Number(send.session);
      if (send.event === 'lay') {
        if (session === queen) lay(send.data.eggs);
        return;
      }
      const slot = slotOf[session];
      if (slot < 0 || sessionOf[slot] !== session) return;
      if (send.event === 'forage') forage(slot);
      else if (send.event === 'home') home(slot);
      else if (send.event === 'rest') restInHill(slot);
    },
  };

  const compiled = compileCharts([
    { name: 'ant.scxml', text: charts.ant },
    { name: 'queen.scxml', text: charts.queen },
  ]);
  host = new Host(compiled, {
    system: `anthill-${colony + 1}`,
    clock,
    config: { system: { maxActors: CAP + 64, maxEvents: Math.max(256, Math.ceil(CAP / 4)), maxTimers: Math.max(64, Math.ceil(CAP / 8)), tracing: false } },
    ioProcessors: { 'urn:world': world },
    onDone: (actor) => {
      const session = Number(actor.id);
      const slot = slotOf[session];
      if (slot < 0 || sessionOf[slot] !== session) return;
      if (stage[slot] === BROOD) counts.brood--; else counts.workers--;
      counts.deaths++;
      sessionOf[slot] = 0; slotOf[session] = -1; go(slot, 0);
      free[freeTop++] = slot;
      touch(slot);
    },
  });
  compiled.close();
  queen = Number(host.spawn('queen', { params: { colony } }).id);

  /** Spawns the first population: nine in ten workers, the rest brood. */
  async function populate(progress = () => {}) {
    for (let start = 0; start < CAP; start += CHUNK) {
      for (let i = start; i < Math.min(CAP, start + CHUNK); i++) hatch(random() < 0.9 ? WORKER : BROOD);
      await host.idle();
      progress(Math.min(CAP, start + CHUNK) / CAP);
    }
  }

  /**
   * One frame: the other colony's last frame is taken in, the clock moves
   * on, and each ant that arrived hears of it. Answers this frame's message
   * for the other colony.
   */
  async function step(other = null) {
    const items = [];
    if (other) {
      for (let i = 0; i < FOOD.sources; i++) foodLeft[i] = Math.max(0, foodLeft[i] - other.eaten[i]);
      for (let i = 0; i < other.arrivals.length; i += 4) {
        const cell = other.arrivals[i];
        enemyAt[cell] = other.arrivals[i + 1]; enemySlot[cell] = other.arrivals[i + 2]; enemySession[cell] = other.arrivals[i + 3];
      }
      for (let i = 0; i < other.struck.length; i += 2) {
        const slot = other.struck[i], session = other.struck[i + 1];
        if (sessionOf[slot] === session && outside[slot]) items.push({ target: session, event: 'fight' });
      }
    }
    // Food a reader placed, in colony order, the same frame in both.
    const mine = placed;
    placed = [];
    for (const [x, y] of colony === 0 ? [...mine, ...(other?.placed ?? [])] : [...(other?.placed ?? []), ...mine]) addFood(x, y);

    clock.increment(FRAME);
    const t = now();
    const bucket = buckets.get(clock.now / FRAME) ?? [];
    buckets.delete(clock.now / FRAME);
    eaten = new Int32Array(FOOD.sources);
    const arrivals = [], struck = [];
    for (let i = 0; i < bucket.length; i += 2) {
      const slot = bucket[i], session = bucket[i + 1];
      if (sessionOf[slot] !== session) continue; // ended, or a newer ant in the slot
      let kind = pending[slot];
      if (kind === FOUND) {
        const food = target[slot];
        if (food >= 0 && foodLeft[food] > 0) { foodLeft[food]--; eaten[food]++; } else kind = ARRIVE;
      }
      if (kind === ARRIVE && carrying[slot]) { carrying[slot] = 0; counts.harvest++; counts.stored++; }
      items.push({ target: session, event: EVENTS[kind] });
      if (outside[slot] && kind !== DIE) meet(slot, t, items, arrivals, struck);
    }
    host.sendMany(items);
    messages += items.length;
    if (clock.now % (FOOD.every * 1000) === 0) sprout();
    if (clock.now % 1000 === 0) {
      if (counts.harvest > 0) { host.send(queen, 'harvest', { food: counts.harvest }); messages++; }
      counts.harvest = 0;
    }
    await host.idle();
    return { arrivals: Float64Array.from(arrivals), struck: Float64Array.from(struck), eaten, placed: mine };
  }

  // An ant that arrives in a cell where an enemy arrived less than half a
  // second before meets it, and may fight: this colony tells its own ant,
  // and the message tells the other colony to tell the enemy.
  function meet(slot, t, items, arrivals, struck) {
    const cell = Math.floor(y1[slot] / CELL) * COLS + Math.floor(x1[slot] / CELL);
    arrivals.push(cell, t, slot, sessionOf[slot]);
    if (t - enemyAt[cell] < MEETING && random() < FIGHT) {
      items.push({ target: sessionOf[slot], event: 'fight' });
      struck.push(enemySlot[cell], enemySession[cell]);
      counts.fights++;
      enemyAt[cell] = -1e9;
    }
  }

  /** Food at (x, y), placed in both colonies next frame. */
  function placeFood(x, y) { placed.push([x, y]); }

  /** Kills this colony's ants outside within `radius` of (x, y), now; answers how many. */
  function squash(x, y, radius) {
    const t = now();
    let killed = 0;
    for (let slot = 0; slot < CAP; slot++) {
      if (!sessionOf[slot] || !outside[slot]) continue;
      const [px, py] = position(slot, t);
      if ((px - x) ** 2 + (py - y) ** 2 > radius * radius) continue;
      host.send(sessionOf[slot], 'kill');
      killed++; messages++;
    }
    return killed;
  }

  function position(slot, t) {
    const span = t1[slot] - t0[slot];
    const f = span > 0 ? Math.min(1, Math.max(0, (t - t0[slot]) / span)) : 1;
    return [x0[slot] + (x1[slot] - x0[slot]) * f, y0[slot] + (y1[slot] - y0[slot]) * f];
  }

  /** The trips that changed since the last call: [slot, x0, y0, x1, y1, t0, t1, look] each, look 0 hidden, 1 or 2 its colony. */
  function delta() {
    const out = new Float32Array(changes * 8);
    for (let i = 0; i < changes; i++) {
      const slot = changed[i];
      marked[slot] = 0;
      const at = i * 8;
      out[at] = offset + slot; out[at + 1] = x0[slot]; out[at + 2] = y0[slot]; out[at + 3] = x1[slot]; out[at + 4] = y1[slot];
      out[at + 5] = t0[slot]; out[at + 6] = t1[slot]; out[at + 7] = sessionOf[slot] && outside[slot] ? colony + 1 : 0;
    }
    changes = 0;
    return out;
  }

  function stats() {
    const { workers, brood, out, stored, deaths, born, fights } = counts;
    return {
      time: now(),
      decisions,
      messages,
      colony: { workers, brood, out, stored, deaths, born },
      fights,
      food: Array.from({ length: foods }, (_, i) => [foodX[i], foodY[i], foodLeft[i]]),
    };
  }

  /** Rests from now on last `seconds` on average. */
  function setRest(seconds) { resting = seconds; }

  return { populate, step, squash, placeFood, setRest, delta, stats, close: () => host.close(), capacity: CAP };
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
