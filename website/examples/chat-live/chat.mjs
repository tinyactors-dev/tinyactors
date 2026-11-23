// The team chat of the tutorial (examples/chat), finished: a workspace
// that makes rooms, users who come and go, and a store that keeps idle
// actors out of memory, on step 8's charts (typing, a grace period for a
// lost connection, a room's history) or step 5's (none of them). On step
// 8 a channel's history is data this program stores (`history` below):
// the room chart sends each post there, the store numbers it, and a screen
// reads a channel's latest posts from the store when it joins. It takes the
// tinyactors package as an argument, so the same code runs in Node.js
// (main.mjs, against output.txt) and in the reader's browser (page.mjs, on @tinyactors/wasm).
//
// What each user's screen shows is built only from what the charts send to
// `chat:client` for that user: the messages, the roster and its changes,
// who is typing, the rooms joined and the errors. The page draws it;
// nothing here decides what a user sees.
//
// The experiments are what an application around the charts does to them:
// it loses a person's connection and gets it back later, it fails or
// delays what reaches a screen, and it moves the running chat to the other
// version of the charts with a live migration.

// The people of the workspace, The Hill: an ant colony's castes, each a
// different initial for the page's avatars.
export const WORKSPACE = 'The Hill';
export const PEOPLE = ['queen', 'scout', 'forager', 'nurse', 'guard', 'drone'];
export const ROOMS = ['general', 'random'];
export const RESIDENT = 4; // actors the host keeps in memory at rest
/** The versions of the charts the chat runs: the tutorial's steps. */
export const VERSIONS = {
  5: 'Step 5: no typing indicator; a lost connection is offline at once; a late joiner sees no history',
  8: 'Step 8: typing indicators; a lost connection has 30 seconds to come back; a late joiner sees the history',
};
/** The newest version: what steps 6 to 8 added is there. */
export const LATEST = 8;
const KEEP = 200; // the newest messages a screen keeps, and entries the log keeps

/** The scripted experiments, each a list of [seconds from its start, what happens]. */
export const SCENARIOS = {
  tunnel: {
    title: 'Scout walks through a tunnel',
    about: 'Scout loses the connection three times, for 8, 12 and 40 seconds, while forager posts.',
    steps: [
      [0, (chat) => chat.loseConnection('scout', 8)],
      [2, (chat) => chat.post('forager', 'general', 'scout, are you there?')],
      [20, (chat) => chat.loseConnection('scout', 12)],
      [25, (chat) => chat.post('forager', 'general', 'the sugar is by the oak')],
      [45, (chat) => chat.loseConnection('scout', 40)],
      [50, (chat) => chat.post('forager', 'general', 'bring two more')],
    ],
  },
  typing: {
    title: 'Everyone types at once',
    about: 'Queen, scout, nurse and guard type in general for 8 seconds; then queen posts.',
    steps: [
      ...[0, 2, 4, 6, 8].flatMap((at) => ['queen', 'scout', 'nurse', 'guard'].map((name) => [at, (chat) => chat.typing(name, 'general')])),
      [9, (chat) => chat.post('queen', 'general', 'everyone to the nursery')],
    ],
  },
};
const ONLINE_FOR = { tunnel: ['scout', 'forager'], typing: ['queen', 'scout', 'nurse', 'guard', 'forager'] };

/** A store that keeps hibernated actors' blobs in a Map, so the page can say which are saved. */
class Store {
  blobs = new Map();
  save(session, blob) { this.blobs.set(String(session), blob); }
  load(session) { return this.blobs.get(String(session)); }
  delete(session) { this.blobs.delete(String(session)); }
}

/** A seeded generator, so a run with faults prints the same every time. */
function generator(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * The workspace, its rooms and PEOPLE, on a host with a simulated clock.
 * `versions` maps 5 and 8 to that step's charts (file name to text);
 * `version` is the one it starts on. Every action waits for the host to
 * settle, one at a time, and then calls `onChange`.
 */
export async function createChat(tinyactors, { versions, version = LATEST, seed = 1, onChange = () => {} } = {}) {
  const { compileCharts, Host, SimulatedClock, Spawner } = tinyactors;
  const store = new Store();
  const clock = new SimulatedClock();
  const now = () => clock.now / 1000;
  const minutes = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
  const random = generator(seed);

  // ---- what happened, newest last: inputs, what reached a screen, faults, versions
  const log = [];
  const note = (kind, text) => {
    log.push({ at: now(), kind, text });
    if (log.length > KEEP) log.shift();
  };

  // ---- the simulated clock's own agenda: what the application does later
  const agenda = []; // { at (ms), what, run }
  const later = (seconds, what, work) => {
    const entry = { at: clock.now + seconds * 1000, what, run: work };
    agenda.push(entry);
    agenda.sort((a, b) => a.at - b.at);
    return entry;
  };

  // ---- each user's screen, and the faults between the charts and it
  const screens = new Map();
  const faults = new Map(); // user -> { drop: 0..1, delay: seconds }
  const screen = (user) => {
    let found = screens.get(user);
    // newest: room -> the newest post number the screen has of it (step 8's), from a join on
    if (!found) screens.set(user, (found = { rooms: [], messages: [], newest: {}, roster: {}, typing: {}, notices: [] }));
    return found;
  };
  /**
   * Adds to `user`'s screen the posts of `room` it lacks, in the store's
   * order: each before the first numbered post after it, each at the time
   * the store recorded for it. Answers the posts it added.
   */
  const fill = (user, room, posts) => {
    const target = screen(user);
    const missing = posts.filter((post) => !target.messages.some((m) => m.room === room && m.seq === post.seq));
    for (const post of posts) target.newest[room] = Math.max(target.newest[room] ?? 0, post.seq);
    if (missing.length === 0) return missing;
    const others = target.messages.filter((m) => m.room !== room);
    const here = target.messages.filter((m) => m.room === room);
    for (const post of missing) {
      const index = here.findIndex((m) => m.seq !== undefined && m.seq > post.seq);
      here.splice(index < 0 ? here.length : index, 0, { room, seq: post.seq, from: post.from, text: post.text, at: post.at });
    }
    target.messages = [...others, ...here].slice(-KEEP);
    return missing;
  };
  /** Reads from the store the posts of `room` numbered after `after` and before `before` that `user`'s screen lacks. */
  const recover = (user, room, after, before = Infinity) => {
    const added = fill(user, room, history.read(room).filter((post) => post.seq > after && post.seq < before));
    if (added.length > 0) note('data', `${user}'s screen reads from the store what it lacks of #${room}: ${added.map((post) => `post ${post.seq}`).join(', ')}`);
  };
  /**
   * When `name`'s connection opens, their screen reads from the store, for
   * each room it has joined, the posts it lacks: those made while they
   * were signed out or offline, and those a fault dropped. Step 5's posts
   * are not in the store, so on step 5 there is nothing to read.
   */
  const catchUp = (name) => {
    if (current !== LATEST) return;
    const target = screen(name);
    for (const room of target.rooms) {
      if (target.newest[room] !== undefined) recover(name, room, target.newest[room]);
    }
  };
  const show = (event, data) => {
    const target = screen(data.to);
    if (event === 'message') {
      // step 8 numbers each post: a screen shows one once (it came with a join's history too)
      if (data.seq !== undefined && target.messages.some((m) => m.room === data.room && m.seq === data.seq)) return;
      // and a screen more than one post behind reads the gap from the store first
      if (data.seq !== undefined) {
        const newest = target.newest[data.room];
        if (newest !== undefined && data.seq > newest + 1) recover(data.to, data.room, newest, data.seq);
        target.newest[data.room] = Math.max(newest ?? 0, data.seq);
      }
      // step 8's posts carry the time the store recorded, so one held or delayed on the way shows
      // when it was posted; step 5's are not stored and carry none: the screen shows when it came
      target.messages.push({ room: data.room, seq: data.seq, from: data.from, text: data.text, at: data.at ?? now() });
      if (target.messages.length > KEEP) target.messages.shift();
    } else if (event === 'roster') target.roster = { ...data.roster };
    else if (event === 'roster.update') target.roster = { ...target.roster, [data.user]: data.status };
    else if (event === 'typing') target.typing = { ...target.typing, [data.room]: [...new Set([...(target.typing[data.room] ?? []), data.user])] };
    else if (event === 'typing.stopped') target.typing = { ...target.typing, [data.room]: (target.typing[data.room] ?? []).filter((user) => user !== data.user) };
    else if (event === 'joined') {
      if (!target.rooms.includes(data.room)) target.rooms = [...target.rooms, data.room];
      // on step 8 a join reads the channel's latest posts from the store, and the screen adds those it
      // lacks, by the store's numbers, at the store's times; step 5 keeps none
      if (current === LATEST) {
        target.newest[data.room] ??= 0;
        fill(data.to, data.room, history.read(data.room));
      }
    }
    else if (event === 'error') target.notices.push(data.reason);
  };
  const describe = (event, data) => {
    if (event === 'message') return `${data.from} in #${data.room}: ${data.text}`;
    if (event === 'roster.update') return `${data.user} is ${data.status}`;
    if (event === 'typing') return `${data.user} is typing in #${data.room}`;
    if (event === 'typing.stopped') return `${data.user} stopped typing in #${data.room}`;
    if (event === 'roster') return 'the whole roster';
    if (event === 'joined') return `joined #${data.room}${current === LATEST ? ', reads the latest posts from the store' : ''}`;
    return `${event}${data.reason ? `: ${data.reason}` : ''}`;
  };
  const client = {
    receive(send) {
      const event = send.event, data = { ...send.data }; // read now: the view ends with the batch
      const fault = faults.get(data.to) ?? { drop: 0, delay: 0 };
      if (fault.drop > 0 && random() < fault.drop) {
        send.fail(); // the sender gets error.communication; the screen never sees it
        note('lost', `${data.to}'s screen lost: ${describe(event, data)}`);
        return;
      }
      if (fault.delay > 0) {
        note('late', `${data.to}'s screen gets it in ${fault.delay} s: ${describe(event, data)}`);
        later(fault.delay, `deliver to ${data.to}`, () => show(event, data));
        return;
      }
      note('screen', `${data.to} sees ${describe(event, data)}`);
      show(event, data);
    },
  };

  // ---- the channels' history, step 8's: data this program stores, a list
  // of posts a channel. The room chart sends each post here (chat:history);
  // the store numbers it, per channel, records when it was made (the
  // simulated clock, in seconds) and answers `appended` with both, which
  // the room puts on the post to each member. The store holds
  // the count, so a switch of chart versions never starts it again. A real
  // application keeps it in a database table.
  const history = {
    posts: new Map(), // room -> [{ seq, at, from, text }]
    receive(send) {
      const { room, from, text } = send.data;
      if (!this.posts.has(room)) this.posts.set(room, []);
      const posts = this.posts.get(room);
      const seq = posts.length + 1;
      const at = now();
      posts.push({ seq, at, from, text });
      note('data', `#${room}'s history stores post ${seq} from ${from}`);
      send.answer('appended', { seq, at, room, from, text });
    },
    /** The latest `limit` posts of `room`. */
    read(room, limit = 50) {
      return (this.posts.get(room) ?? []).slice(-limit);
    },
  };

  // ---- the host, on the version asked for; both versions compiled once
  const images = {};
  for (const [step, charts] of Object.entries(versions)) {
    const compiled = compileCharts(Object.entries(charts).map(([name, text]) => ({ name, text })));
    images[step] = compiled.image;
    compiled.close();
  }
  let current = version;
  let migrating = null; // resolves with a migration's result
  const host = new Host(images[current], {
    system: 'chat',
    clock,
    store,
    resident: RESIDENT,
    ioProcessors: { 'chat:client': client, 'chat:history': history, spawn: new Spawner() },
    migrationDone: (result) => migrating?.(result),
  });

  let queue = Promise.resolve();
  /**
   * Runs `work` after the calls before it, then lets the host settle; an
   * action (`changes`) then calls onChange, a read does not, so a page that
   * reads on each change does not loop.
   */
  const run = (work, changes) => {
    const done = queue.then(async () => {
      const result = await work();
      await host.idle();
      if (changes) onChange();
      return result;
    });
    queue = done.catch(() => {});
    return done;
  };
  const act = (work) => run(work, true);
  const read = (work) => run(work, false);
  const user = (name) => host.actor(`user.${name}`);
  /** Sends `event` to `name`'s user actor and logs it. */
  const tell = (name, event, data) => {
    note('input', `${name} ← ${event}${data?.room ? ` #${data.room}` : ''}${data?.text ? `: ${data.text}` : ''}`);
    user(name).send(event, data);
  };

  // ---- the connections: each the socket a person's page would hold. The
  // application opens one when the person signs in and closes it when they
  // sign out; a lost one is gone until the reconnect the agenda holds.
  const connections = new Set(); // people with a connection
  const losses = new Map(); // person -> the agenda's entry that reconnects them
  /** Cancels `name`'s pending reconnect, if any, saying why. */
  const cancelReconnect = (name, why) => {
    const entry = losses.get(name);
    if (!entry) return;
    losses.delete(name);
    agenda.splice(agenda.indexOf(entry), 1);
    note('fault', `${name}'s reconnect at ${minutes(entry.at / 1000)} is off: ${why}`);
  };
  /** `name` signs in: a connection opens (early, if a lost one was to come back later). */
  const open = (name) => {
    cancelReconnect(name, `${name} signed in again first`);
    connections.add(name);
    catchUp(name);
    tell(name, 'connect');
  };
  /** `name` signs out: the connection closes, and a lost one does not come back. */
  const close = (name) => {
    cancelReconnect(name, `${name} signed out`);
    connections.delete(name);
    tell(name, 'disconnect');
  };
  /**
   * `name`'s connection drops, and comes back `seconds` later. Step 8's
   * charts get `connection.lost`; step 5's know only `disconnect`, so an
   * application on them can say nothing else. A person with no connection
   * (signed out, or lost already) has none to lose.
   */
  const lose = (name, seconds) => {
    if (!connections.has(name)) {
      note('fault', losses.has(name) ? `${name}'s connection is lost already` : `${name} has no connection to lose`);
      return;
    }
    connections.delete(name);
    tell(name, current === LATEST ? 'connection.lost' : 'disconnect');
    losses.set(name, later(seconds, `${name} reconnects`, () => {
      losses.delete(name);
      connections.add(name);
      catchUp(name);
      tell(name, 'connect');
    }));
  };

  const workspace = host.spawn('workspace', { name: 'workspace' });
  for (const name of PEOPLE) host.spawn('user', { name: `user.${name}`, params: { name } });
  await host.idle();
  for (const room of ROOMS) await workspace.ask('room.create', { name: room });
  await host.idle();
  log.length = 0;

  /** Moves the clock `ms` on, doing what the agenda holds on the way, each at its time. */
  async function advance(ms) {
    const end = clock.now + ms;
    while (agenda.length > 0 && agenda[0].at <= end) {
      const next = agenda.shift();
      if (next.at > clock.now) clock.increment(next.at - clock.now);
      await host.idle();
      await next.run();
      await host.idle();
    }
    if (end > clock.now) clock.increment(end - clock.now);
  }

  const chat = {
    /** What `user`'s screen shows: rooms joined, messages, the roster, who is typing, errors. */
    screen,
    /** The simulated time, in seconds. */
    now,
    connect: (name) => act(() => open(name)),
    disconnect: (name) => act(() => close(name)),
    /** Whether `name` has a connection: signed in, and not lost. */
    connected: (name) => connections.has(name),
    /** When `name`'s lost connection comes back (simulated seconds), or null when none is lost. */
    reconnects: (name) => (losses.has(name) ? losses.get(name).at / 1000 : null),
    /** Typing: on step 8 the room hears of it; on both the user's away timer starts again. */
    typing: (name, room) => act(() => tell(name, current === LATEST ? 'typing' : 'activity', current === LATEST ? { room } : undefined)),
    join: (name, room) => act(() => tell(name, 'join', { room })),
    post: (name, room, text) => act(() => tell(name, 'post', { room, text })),
    /** Asks the workspace for a room: answers [event, data], room.created or room.refused. */
    createRoom: (name) => act(async () => {
      note('input', `workspace ← room.create #${name}`);
      return workspace.ask('room.create', { name });
    }),
    /** The rooms the workspace holds. */
    rooms: () => read(async () => (await workspace.ask('rooms.list'))[1].rooms),
    /** Moves the simulated clock `ms` on: due timers fire, and what the agenda holds happens. */
    advance: (ms) => act(() => advance(ms)),

    // ---- experiments
    /** `name`'s connection drops, and comes back `seconds` later; nothing happens to a person with none. */
    loseConnection: (name, seconds) => act(() => lose(name, seconds)),
    /** Faults between the charts and `name`'s screen: a share of deliveries dropped, and a delay in seconds. */
    setFaults: (name, { drop = 0, delay = 0 } = {}) => act(() => {
      if (drop === 0 && delay === 0) faults.delete(name);
      else faults.set(name, { drop, delay });
      note('fault', drop === 0 && delay === 0 ? `${name}'s screen is reliable again`
        : `${name}'s screen${drop ? ` loses ${Math.round(drop * 100)}%` : ''}${drop && delay ? ' and' : ''}${delay ? ` gets each delivery ${delay} s late` : ''}`);
    }),
    faults: (name) => faults.get(name) ?? { drop: 0, delay: 0 },
    /** Runs scenario `id` (SCENARIOS): its people come online in #general, then its steps go on the agenda. */
    scenario: (id) => act(async () => {
      const scenario = SCENARIOS[id];
      note('scenario', `${scenario.title}: ${scenario.about}`);
      for (const name of ONLINE_FOR[id]) {
        open(name);
        tell(name, 'join', { room: 'general' });
      }
      await host.idle();
      for (const [at, step] of scenario.steps) later(at, scenario.title, () => step(direct));
    }),
    /** What the application will do, and when (simulated seconds). */
    agenda: () => agenda.map((entry) => ({ at: entry.at / 1000, what: entry.what })),
    /** What happened, newest last: { at, kind (input, screen, lost, late, fault, scenario, version, data), text }. */
    log: () => log.slice(),

    // ---- the version of the charts
    version: () => current,
    /**
     * Moves the running chat to step `step`'s charts: the host adds that
     * version's image, and a live migration moves every actor onto it, the
     * hibernated ones loaded as room allows. Answers the migration's result
     * ({migrated, failed, pending}).
     */
    useVersion: (step) => act(async () => {
      if (step === current) return null;
      host.addImage(images[step]);
      const done = new Promise((resolve) => { migrating = resolve; });
      host.migrate({ wake: true });
      // the migration moves actors in later turns, on the host's clock
      let result = null;
      done.then((value) => { result = value; });
      for (let tries = 0; tries < 200 && !result; tries++) {
        clock.increment(10);
        await host.idle();
      }
      migrating = null;
      current = step;
      // step 5 sends no typing.stopped: indicators from before the switch would stay
      if (step !== LATEST) for (const target of screens.values()) target.typing = {};
      note('version', `moved to ${VERSIONS[step].split(':')[0]}: ${result?.migrated ?? 0} actors migrated, ${result?.failed?.length ?? 0} failed`);
      return result;
    }),

    /** Every actor by name, with whether it is in memory or saved in the store. */
    actors: () => read(async () => {
      const names = ['workspace', ...(await workspace.ask('rooms.list'))[1].rooms.map((room) => `room.${room}`), ...PEOPLE.map((name) => `user.${name}`)];
      return names.map((name) => {
        const actor = host.actor(name);
        return { name, chart: name.split('.')[0], session: actor.id, stored: store.blobs.has(String(actor.id)) };
      });
    }),
    /**
     * One actor's state as its host reports it (actor.state's value), or
     * `stored: true` while it waits in the store: its state is in the blob,
     * and reading it does not wake it.
     */
    state: (name) => read(async () => {
      const actor = host.actor(name);
      if (store.blobs.has(String(actor.id))) return { stored: true };
      try {
        return { stored: false, value: (await actor.state()).value };
      } catch {
        return { stored: true }; // saved while we asked
      }
    }),
    /** How many actors there are, and how many are in memory. */
    stats: () => read(async () => {
      const { host: stats } = await host.hostStats();
      return { actors: stats.actors, inMemory: stats.actors - stats.hibernated, stored: store.blobs.size };
    }),
    close: () => host.close(),
  };

  // What the agenda runs is inside an action already (advance's): these
  // send at once, without queueing behind the action that runs them.
  const direct = {
    loseConnection: lose,
    post: (name, room, text) => tell(name, 'post', { room, text }),
    typing: (name, room) => tell(name, current === LATEST ? 'typing' : 'activity', current === LATEST ? { room } : undefined),
  };
  return chat;
}
