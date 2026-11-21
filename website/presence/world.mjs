// The field the visitors' ants walk: their actors (visitor.scxml) on one
// host, and the world as their I/O processor, urn:world. An ant decides
// only when something happens to it; the world picks each trip (two points,
// two times), tells the ant when it arrives, and keeps what the ants leave:
// a pheromone grid per country, and the food whose scent spreads. An ant
// that smells food walks to it and eats a bite (the pile shrinks, and goes
// when it is finished); two roaming ants that meet greet. Between
// decisions nothing runs: every browser draws each ant on its trip.
//
// Positions are fractions of the field, 0 to 1 on each axis; each browser
// maps them to its own viewport. Times are the host's clock in ms.
//
// The world takes the tinyactors package as an argument, so the same code
// runs in the server (@tinyactors/node, the runtime's clock) and in
// examples/presence (a SimulatedClock, checked against output.txt).

export const SPEED = 0.03;          // field widths a second: a stroll
export const REACH = 0.045;         // in reach of a greeting
export const FACE = 0.032;          // how far apart two greeting ants stand
const HOP = { min: 0.05, max: 0.16 }; // a roaming trip's length
const GRID = { cols: 48, rows: 30 };
const DEPOSIT = 0.35, EVAPORATE = 0.96; // per cell crossed; per second
const FOOD = { sources: 5, amount: 24, bite: 8, spread: 0.12, smell: 0.28, full: 30_000, regrow: 20_000 };
const BUMP = { reach: 0.03, rest: 30_000 }; // two roaming ants this close greet; then neither does for a while
const MAX_HOP_TOWARD = 0.18;        // an approach is re-planned each leg
const HURRY = 2;                    // an ant on its way to greet walks this many times faster

/**
 * The world and its host. `emit(to, message)` sends a message to one tab
 * (`to` its ant's handle) or to every tab (`to` null); the server turns it
 * into WebSocket frames, the test prints it.
 */
export function createWorld(tinyactors, { chart, clock, random = Math.random, emit = () => {}, config = {} }) {
  const { compileCharts, Host, MemoryStore } = tinyactors;
  const ants = new Map();      // session (number) -> ant
  const byHandle = new Map();  // handle -> ant
  let nextHandle = 1;
  const trails = new Map();    // country -> Float32Array(cols*rows)
  const food = [];
  const now = () => host.now();
  const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
  const cell = (x, y) => clamp(Math.floor(y * GRID.rows), 0, GRID.rows - 1) * GRID.cols + clamp(Math.floor(x * GRID.cols), 0, GRID.cols - 1);

  // ---- where an ant is, on its trip
  function position(ant, t = now()) {
    const span = ant.t1 - ant.t0;
    const f = span > 0 ? clamp((t - ant.t0) / span, 0, 1) : 1;
    return [ant.x0 + (ant.x1 - ant.x0) * f, ant.y0 + (ant.y1 - ant.y0) * f];
  }
  const record = (ant) => [ant.handle, round(ant.x0), round(ant.y0), ant.t0, round(ant.x1), round(ant.y1), ant.t1, ant.face ?? null];
  const round = (x) => Math.round(x * 10000) / 10000;

  // ---- arrivals, kept in order of time; the world's tick hands them out.
  // Each trip has a number: an arrival of a trip that a later one replaced
  // (a greeting stopped the ant) is dropped.
  let due = []; // [t, session, event, data, trip]
  function trip(ant, x, y, { event = 'arrive', data, face = null, seconds } = {}) {
    const t = now();
    const [px, py] = position(ant, t);
    x = clamp(x, 0.02, 0.98); y = clamp(y, 0.03, 0.97);
    const distance = Math.hypot(x - px, y - py);
    const ms = Math.round((seconds ?? distance / SPEED) * 1000);
    Object.assign(ant, { x0: px, y0: py, t0: t, x1: x, y1: y, t1: t + ms, face });
    ant.trip++;
    lay(ant, px, py, x, y);
    if (event) due.push([t + ms, ant.session, event, data, ant.trip]);
    emit(null, { t: 'trip', trip: record(ant) });
  }
  const halt = (ant, face = null) => { const [x, y] = position(ant); trip(ant, x, y, { event: null, face, seconds: 0 }); };

  // ---- pheromones: an ant lays its country's trail along each trip
  function trail(country) {
    let grid = trails.get(country);
    if (!grid) trails.set(country, (grid = new Float32Array(GRID.cols * GRID.rows)));
    return grid;
  }
  function lay(ant, x0, y0, x1, y1) {
    const grid = trail(ant.country);
    const steps = Math.max(1, Math.ceil(Math.hypot((x1 - x0) * GRID.cols, (y1 - y0) * GRID.rows)));
    for (let i = 0; i <= steps; i++) {
      const c = cell(x0 + ((x1 - x0) * i) / steps, y0 + ((y1 - y0) * i) / steps);
      grid[c] = Math.min(1, grid[c] + DEPOSIT / steps * 2);
    }
  }
  function evaporate() {
    for (const [country, grid] of trails) {
      let left = 0;
      for (let i = 0; i < grid.length; i++) left += (grid[i] *= EVAPORATE);
      if (left < 0.01) trails.delete(country);
    }
  }

  // ---- food: a few piles whose scent spreads; eaten a bite at a time, and
  // when one is finished another grows elsewhere a while later
  let nextFood = 1;
  const sprout = () => ({ id: nextFood++, x: 0.1 + random() * 0.8, y: 0.12 + random() * 0.76, amount: FOOD.amount, gone: 0 });
  for (let i = 0; i < FOOD.sources; i++) food.push(sprout());
  const piles = () => food.filter((f) => f.amount > 0).map((f) => [f.id, round(f.x), round(f.y), f.amount / FOOD.amount]);
  const foodChanged = () => emit(null, { t: 'food', food: piles() });
  const scent = (x, y) => food.reduce((s, f) => s + (f.amount / FOOD.amount) * Math.exp(-((x - f.x) ** 2 + (y - f.y) ** 2) / (2 * FOOD.spread ** 2)), 0);

  // ---- what an ant asks for
  function roam(ant) {
    const [x, y] = position(ant);
    // Hungry, and a pile in smelling distance: most times, walk to it.
    if (now() >= ant.fullUntil) {
      let near = null, nearest = FOOD.smell;
      for (const f of food) {
        const d = Math.hypot(f.x - x, f.y - y);
        if (f.amount > 0 && d < nearest) { near = f; nearest = d; }
      }
      if (near && random() < 0.6) {
        const stop = Math.max(0, nearest - 0.012); // just short of the pile, facing it
        const tx = x + ((near.x - x) / (nearest || 1)) * stop, ty = y + ((near.y - y) / (nearest || 1)) * stop;
        ant.heading = Math.atan2(near.y - y, near.x - x);
        return trip(ant, tx, ty, { event: 'found', data: { food: near.id }, face: [round(near.x), round(near.y)] });
      }
    }
    // Eight directions tried; each scored by its own country's trail, the
    // food's scent, a pull from the edges, and chance.
    const mine = trails.get(ant.country);
    let best = null, bestScore = -Infinity;
    for (let i = 0; i < 8; i++) {
      const angle = ant.heading + (random() - 0.5) * Math.PI * 1.4;
      const hop = HOP.min + random() * (HOP.max - HOP.min);
      const tx = x + Math.cos(angle) * hop, ty = y + Math.sin(angle) * hop;
      if (tx < 0.02 || tx > 0.98 || ty < 0.03 || ty > 0.97) continue;
      const score = 1.6 * (mine ? mine[cell(tx, ty)] : 0) + 1.2 * scent(tx, ty) + 0.9 * random();
      if (score > bestScore) { bestScore = score; best = [tx, ty, angle]; }
    }
    if (!best) { // cornered: back toward the middle
      const angle = Math.atan2(0.5 - y, 0.5 - x);
      best = [x + Math.cos(angle) * HOP.min, y + Math.sin(angle) * HOP.min, angle];
    }
    ant.heading = best[2];
    trip(ant, best[0], best[1]);
  }

  function approach(ant, handle) {
    const other = byHandle.get(handle);
    if (!other || other === ant || other.asleep || other.leaving) return tell(ant, 'lost');
    const [x, y] = position(ant), [ox, oy] = position(other);
    const d = Math.hypot(ox - x, oy - y);
    if (d <= REACH) {
      return tell(ant, 'near', { peer: other.address });
    }
    // Stop FACE short of where the other is, at most a leg at a time.
    const step = Math.min(d - FACE, MAX_HOP_TOWARD);
    trip(ant, x + ((ox - x) / d) * step, y + ((oy - y) / d) * step, { seconds: step / (SPEED * HURRY) });
  }

  const tell = (ant, event, data) => due.push([now(), ant.session, event, data]);

  // An ant's mode, as the world last heard it from its chart. Its tab is
  // told which ant it is walking to (it draws the track there), and when
  // that ends: greeted, lost, given up or interrupted.
  function setMode(ant, mode, target = 0) {
    if (mode === 'approaching' && ant.target !== target) emit(ant.handle, { t: 'target', h: target });
    else if (mode !== 'approaching' && ant.mode === 'approaching') emit(ant.handle, { t: 'target', h: 0 });
    ant.mode = mode;
    ant.target = mode === 'approaching' ? target : 0;
  }

  function eat(ant, id) {
    const f = food.find((pile) => pile.id === id);
    const [x, y] = position(ant);
    if (!f || f.amount <= 0 || Math.hypot(f.x - x, f.y - y) > 0.04) return tell(ant, 'empty');
    halt(ant, [round(f.x), round(f.y)]);
    f.amount = Math.max(0, f.amount - FOOD.bite);
    if (f.amount === 0) f.gone = now();
    ant.ate++;
    ant.fullUntil = now() + FOOD.full;
    emit(null, { t: 'eat', h: ant.handle, food: f.id });
    foodChanged();
  }

  // Two roaming ants that meet greet: one of them sends the other a
  // greeting, as a tap would have it do.
  function bumps(t) {
    const roaming = [...ants.values()].filter((ant) => ant.mode === 'roaming' && !ant.asleep && !ant.leaving && t - ant.bumped > BUMP.rest);
    for (let i = 0; i < roaming.length; i++) {
      const a = roaming[i];
      if (a.mode !== 'roaming') continue;
      const [ax, ay] = position(a, t);
      for (let j = i + 1; j < roaming.length; j++) {
        const b = roaming[j];
        if (b.mode !== 'roaming') continue;
        const [bx, by] = position(b, t);
        if (Math.hypot(ax - bx, ay - by) > BUMP.reach) continue;
        const [from, to] = random() < 0.5 ? [a, b] : [b, a];
        a.mode = b.mode = 'busy';
        a.bumped = b.bumped = t;
        host.send(from.session, 'bump', { peer: to.address });
        break;
      }
    }
  }

  const world = {
    receive(send) {
      const ant = ants.get(Number(send.session));
      if (!ant) return;
      switch (send.event) {
        case 'roam': setMode(ant, 'roaming'); return roam(ant);
        case 'approach': setMode(ant, 'approaching', Number(send.data.target)); return approach(ant, Number(send.data.target));
        case 'eat': setMode(ant, 'eating'); return eat(ant, Number(send.data.food));
        case 'greet': {
          const peer = ants.get(sessionOf(send.data.peer));
          setMode(ant, 'busy');
          if (!peer) return;
          setMode(peer, 'busy');
          const [ax, ay] = position(ant), [px, py] = position(peer);
          halt(ant, [round(px), round(py)]);
          halt(peer, [round(ax), round(ay)]);
          ant.greetings++;
          emit(null, { t: 'greet', a: ant.handle, b: peer.handle });
          return;
        }
        case 'greeted': {
          setMode(ant, 'busy');
          emit(ant.handle, { t: 'greeted', country: send.data.country || null });
          return;
        }
        case 'sleep': {
          ant.asleep = true;
          setMode(ant, 'asleep');
          halt(ant);
          emit(null, { t: 'sleep', h: ant.handle, asleep: true });
          return;
        }
        case 'gone': {
          ant.leaving = true;
          return;
        }
      }
    },
  };
  const sessionOf = (address) => {
    const match = /#_scxml_(\d+)$/.exec(String(address));
    return match ? Number(match[1]) : 0;
  };

  const compiled = compileCharts([{ name: 'visitor.scxml', text: chart }]);
  const store = new MemoryStore();
  const host = new Host(compiled, {
    system: 'presence',
    clock,
    store,
    config: { system: { tracing: false, ...config.system } },
    ioProcessors: { 'urn:world': world },
    onDone: (actor) => {
      const ant = ants.get(Number(actor.id));
      if (!ant) return;
      ants.delete(ant.session);
      byHandle.delete(ant.handle);
      emit(null, { t: 'leave', h: ant.handle });
    },
  });
  compiled.close();

  return {
    host,
    /** A tab joins: its ant, named by the tab's id, at a random place. Answers its handle. */
    join(tab, country) {
      const actor = host.spawn('visitor', { name: tab, params: { country } });
      const x = 0.1 + random() * 0.8, y = 0.1 + random() * 0.8, t = now();
      const ant = {
        session: Number(actor.id), handle: nextHandle++, tab, country, address: actor.address.toString(),
        x0: x, y0: y, t0: t, x1: x, y1: y, t1: t, face: null, heading: random() * Math.PI * 2, trip: 0,
        mode: 'starting', target: 0, asleep: false, leaving: false, greetings: 0, ate: 0, fullUntil: 0, bumped: -Infinity,
      };
      ants.set(ant.session, ant);
      byHandle.set(ant.handle, ant);
      emit(null, { t: 'join', h: ant.handle, country });
      return ant.handle;
    },
    /** The ant of a tab already here (a reload within the grace), or undefined. */
    find(tab) {
      for (const ant of ants.values()) if (ant.tab === tab && !ant.leaving) return ant.handle;
      return undefined;
    },
    /** A tap: `handle`'s ant goes to greet `target`'s. */
    greet(handle, target) { const ant = byHandle.get(handle); if (ant && !ant.asleep) host.send(ant.session, 'greet', { target }); },
    hide(handle) {
      const ant = byHandle.get(handle);
      if (!ant || ant.asleep) return;
      host.send(ant.session, 'hide');
      ant.hibernate = true; // after this turn: host.idle() runs the chart to asleep first
    },
    show(handle) {
      const ant = byHandle.get(handle);
      if (!ant || !ant.asleep) return;
      ant.asleep = false;
      emit(null, { t: 'sleep', h: ant.handle, asleep: false });
      host.send(ant.session, 'show'); // a hibernated actor is woken from the store first
    },
    leave(handle) { const ant = byHandle.get(handle); if (ant) host.send(ant.session, 'leave'); },
    /** Every ant, as a client draws them: [handle, country, asleep, trip]. */
    snapshot: () => [...ants.values()].filter((ant) => !ant.leaving).map((ant) => [ant.handle, ant.country, ant.asleep, record(ant)]),
    /** The field a client draws faintly: the food, and `country`'s trail as bytes 0..255. */
    field(country) {
      const grid = trails.get(country);
      const bytes = grid ? Array.from(grid, (v) => Math.round(v * 255)) : null;
      return { cols: GRID.cols, rows: GRID.rows, trail: bytes, food: piles() };
    },
    /**
     * Hands out what is due by now (arrivals, near, lost), hibernates the
     * actors that fell asleep, and once a second lets the trails evaporate
     * and the food regrow. The server calls it every 100 ms.
     */
    async tick() {
      const t = now();
      if (due.length) {
        due.sort((a, b) => a[0] - b[0]);
        const ready = [];
        while (due.length && due[0][0] <= t) ready.push(due.shift());
        const items = ready.filter(([, session, event, , number]) => event && ants.has(session) && (number === undefined || number === ants.get(session).trip))
          .map(([, session, event, data]) => (data === undefined ? { target: session, event } : { target: session, event, data }));
        if (items.length) host.sendMany(items);
      }
      bumps(t);
      await host.idle();
      for (const ant of ants.values()) {
        if (!ant.hibernate) continue;
        ant.hibernate = false;
        if (!ant.asleep) continue;
        // The store is keyed as the host keys it: by bigint.
        try { await store.save(BigInt(ant.session), host.hibernate(ant.session)); } catch { /* not at rest: stays resident */ }
      }
      if (Math.floor(t / 1000) !== this.second) {
        this.second = Math.floor(t / 1000);
        evaporate();
        let grown = false;
        for (let i = 0; i < food.length; i++) if (food[i].amount <= 0 && t - food[i].gone > FOOD.regrow) { food[i] = sprout(); grown = true; }
        if (grown) foodChanged();
      }
    },
    /** For tests and the server's status: what the world holds. */
    stats() {
      let asleep = 0, ate = 0, greetings = 0;
      for (const ant of ants.values()) { asleep += ant.asleep; ate += ant.ate; greetings += ant.greetings; }
      return { ants: ants.size, asleep, ate, greetings, countries: trails.size, food: food.reduce((s, f) => s + f.amount, 0), piles: piles().length };
    },
    second: -1,
    close() { host.close(); },
  };
}
