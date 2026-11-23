// The team chat of the tutorial (examples/chat), finished: a workspace
// that makes rooms, users who come and go, and a store that keeps idle
// actors out of memory, on step 9's charts (typing, a grace period for a
// lost connection, a room's history, direct messages) or step 5's (none of
// them). From step 8 on a channel's history is data this program stores (`history` below):
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
/** Seconds step 9 waits before a lost connection is offline: `grace.over`'s delay in charts/step-9/user.scxml. */
export const GRACE = 30;
/** The versions of the charts the chat runs: the tutorial's steps. */
export const VERSIONS = {
  5: 'Step 5: no typing indicator; a lost connection is offline at once; a late joiner sees no history',
  9: `Step 9: typing indicators; a lost connection shows as away, with ${GRACE} seconds to come back; a late joiner sees the history; direct messages`,
};
/** The newest version: what steps 6 to 9 added is there. */
export const LATEST = 9;
/**
 * The order every list of messages newest first uses (the DMs view, Activity,
 * search results): by time, newest first, then by `order`, the place a
 * message took on its screen, latest first. A time is to the second, so posts
 * of one second tie on it; the stores number their posts per room, so their
 * numbers do not order posts of two rooms, and step 5's posts have none.
 */
export const newestFirst = (a, b) => b.at - a.at || b.order - a.order;
// The newest messages a screen keeps of each conversation, and entries the log keeps. Per
// conversation, so one busy channel cannot push another's posts off the screen; four times what
// a join reads from the store (history.read), so a screen holds what it read and what came since.
const KEEP = 200;

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
 * `versions` maps 5 and 9 to that step's charts (file name to text);
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

  // ---- the scenarios running: those whose start is queued, and those with steps on the agenda
  const starting = new Set();
  const stepsLeft = (id) => (starting.has(id) ? SCENARIOS[id].steps.length
    : agenda.filter((entry) => entry.what === SCENARIOS[id].title).length || null);

  // ---- each user's screen, and the faults between the charts and it
  const screens = new Map();
  const faults = new Map(); // user -> { drop: 0..1, delay: seconds }
  const screen = (user) => {
    let found = screens.get(user);
    // newest: room -> the newest post number the screen has of it (step 9's), from a join on;
    // dms: the direct messages the charts told it of, { room, with, opened } (opened: by this person);
    // limits: room -> the messages of it the screen keeps, KEEP until the person loads earlier ones;
    // last and first: the order (newestFirst) the screen gave the message it added after all and before all
    if (!found) screens.set(user, (found = { rooms: [], messages: [], newest: {}, limits: {}, roster: {}, typing: {}, notices: [], dms: [], last: 0, first: 0 }));
    return found;
  };
  /** Drops the oldest messages of `room` from a screen past what it keeps of that room. */
  const trim = (target, room) => {
    let over = target.messages.filter((m) => m.room === room).length - (target.limits[room] ?? KEEP);
    if (over > 0) target.messages = target.messages.filter((m) => m.room !== room || over-- <= 0);
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
      // a post read after the newest comes last; one read before (an earlier page) takes a place
      // before the post it precedes, so two posts of one room and one second keep the store's order
      const order = index < 0 ? ++target.last : index === 0 ? --target.first : (here[index - 1].order + here[index].order) / 2;
      here.splice(index < 0 ? here.length : index, 0, { room, seq: post.seq, from: post.from, text: post.text, at: post.at, order });
    }
    target.messages = [...others, ...here];
    trim(target, room);
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
  /**
   * `user`'s screen enters `room`, a channel it joined or a direct message:
   * on step 9 it reads the room's latest posts from the store and adds those
   * it lacks, by the store's numbers, at the store's times; step 5 keeps none.
   */
  const enter = (target, user, room) => {
    if (!target.rooms.includes(room)) target.rooms = [...target.rooms, room];
    if (current !== LATEST) return;
    target.newest[room] ??= 0;
    fill(user, room, history.read(room));
  };
  const show = (event, data) => {
    const target = screen(data.to);
    if (event === 'message') {
      // on step 9 the store numbers each post: a screen shows one once (it came with a join's history too)
      if (data.seq !== undefined && target.messages.some((m) => m.room === data.room && m.seq === data.seq)) return;
      // and a screen more than one post behind reads the gap from the store first
      if (data.seq !== undefined) {
        const newest = target.newest[data.room];
        if (newest !== undefined && data.seq > newest + 1) recover(data.to, data.room, newest, data.seq);
        target.newest[data.room] = Math.max(newest ?? 0, data.seq);
      }
      // step 9's posts carry the time the store recorded, so one held or delayed on the way shows
      // when it was posted; step 5's are not stored and carry none: the screen shows when it came
      target.messages.push({ room: data.room, seq: data.seq, from: data.from, text: data.text, at: data.at ?? now(), order: ++target.last });
      trim(target, data.room);
    } else if (event === 'roster') target.roster = { ...data.roster };
    else if (event === 'roster.update') target.roster = { ...target.roster, [data.user]: data.status };
    else if (event === 'typing') target.typing = { ...target.typing, [data.room]: [...new Set([...(target.typing[data.room] ?? []), data.user])] };
    else if (event === 'typing.stopped') target.typing = { ...target.typing, [data.room]: (target.typing[data.room] ?? []).filter((user) => user !== data.user) };
    else if (event === 'joined') enter(target, data.to, data.room);
    // a direct message you opened, or those the workspace says are yours: each read from the
    // store as a join reads a channel, and listed once you opened it or a post came (directMessages)
    else if (event === 'dm.opened') {
      const known = target.dms.find((dm) => dm.room === data.room);
      if (known) known.opened = true;
      else target.dms = [...target.dms, { room: data.room, with: data.with, opened: true }];
      enter(target, data.to, data.room);
    } else if (event === 'dms') {
      for (const dm of data.dms) {
        if (target.dms.some((known) => known.room === dm.room)) continue;
        target.dms = [...target.dms, { room: dm.room, with: dm.with, opened: false }];
        enter(target, data.to, dm.room);
      }
    }
    else if (event === 'error') target.notices.push(data.reason);
  };
  const describe = (event, data) => {
    if (event === 'message') return `${data.from} in ${where(data.room)}: ${data.text}`;
    if (event === 'dm.opened') return `the DM with ${data.with}, reads its latest posts from the store`;
    if (event === 'dms') return `their DMs: ${data.dms.map((dm) => dm.with).join(', ') || 'none'}`;
    if (event === 'roster.update') return `${data.user} is ${data.status}`;
    if (event === 'typing') return `${data.user} is typing in ${where(data.room)}`;
    if (event === 'typing.stopped') return `${data.user} stopped typing in ${where(data.room)}`;
    if (event === 'roster') return 'the whole roster';
    if (event === 'joined') return `joined #${data.room}${current === LATEST ? ', reads the latest posts from the store' : ''}`;
    return `${event}${data.reason ? `: ${data.reason}` : ''}`;
  };
  // a room's name in the log: a channel's with its #, a direct message's as it is (dm. and both names)
  const where = (room) => (room.startsWith('dm.') ? room : `#${room}`);
  const directRooms = new Map(); // the direct messages' rooms the charts named -> their members
  const client = {
    receive(send) {
      const event = send.event, data = { ...send.data }; // read now: the view ends with the batch
      if (event === 'dm.opened') directRooms.set(data.room, [data.to, data.with]);
      if (event === 'dms') for (const dm of data.dms) directRooms.set(dm.room, [data.to, dm.with]);
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

  // ---- the channels' history, step 9's (since step 8): data this program stores, a list
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
      note('data', `${where(room)}'s history stores post ${seq} from ${from}`);
      send.answer('appended', { seq, at, room, from, text });
    },
    /** The latest `limit` posts of `room` numbered before `before`. */
    read(room, limit = 50, before = Infinity) {
      return (this.posts.get(room) ?? []).filter((post) => post.seq < before).slice(-limit);
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
    note('input', `${name} ← ${event}${data?.room ? ` ${where(data.room)}` : ''}${data?.with ? ` with ${data.with}` : ''}${data?.text ? `: ${data.text}` : ''}`);
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
   * `name`'s connection drops, and comes back `seconds` later. Step 9's
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
  // who created each channel the page made, and when: the workspace's answer reaches the asker only
  const created = new Map();
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
    /** Typing: on step 9 the room hears of it; on both the user's away timer starts again. */
    typing: (name, room) => act(() => tell(name, current === LATEST ? 'typing' : 'activity', current === LATEST ? { room } : undefined)),
    join: (name, room) => act(() => tell(name, 'join', { room })),
    post: (name, room, text) => act(() => tell(name, 'post', { room, text })),
    /**
     * Opens `name`'s direct message with `other` (`name` again: their own
     * notes): `name`'s user actor asks the workspace, which makes it once
     * per pair, and the screen hears `dm.opened`. Answers its room, or null
     * on step 5, whose charts have none, or when the screen did not hear.
     */
    openDM: (name, other) => act(async () => {
      if (current !== LATEST) return null;
      tell(name, 'dm.open', { with: other });
      await host.idle();
      return screen(name).dms.find((dm) => dm.with === other && dm.opened)?.room ?? null;
    }),
    /**
     * `name`'s direct messages as a list shows them, newest first: those
     * they opened and those with a post on their screen, each with its
     * room, the other person and its last message (null for none).
     */
    directMessages: (name) => {
      const target = screen(name);
      // one with no message yet (opened) goes last, in the order it was opened
      return target.dms.map((dm) => ({ room: dm.room, with: dm.with, opened: dm.opened, last: target.messages.findLast((m) => m.room === dm.room) ?? null }))
        .filter((dm) => dm.opened || dm.last)
        .sort((a, b) => (a.last && b.last ? newestFirst(a.last, b.last) : Boolean(b.last) - Boolean(a.last)));
    },
    /**
     * `name`'s screen reads from the store the posts of `room` before the
     * oldest numbered one it shows, as a client loads history when you
     * scroll up, and keeps them beside the newest. Answers how many it
     * added: none on step 5, whose charts store no posts, or at post 1.
     */
    loadEarlier: (name, room) => act(() => {
      if (current !== LATEST) return 0;
      const target = screen(name);
      const oldest = target.messages.find((m) => m.room === room && m.seq !== undefined)?.seq;
      if (!oldest || oldest <= 1) return 0;
      const posts = history.read(room, 50, oldest);
      target.limits[room] = Math.max(KEEP, target.messages.filter((m) => m.room === room).length + posts.length);
      const added = fill(name, room, posts);
      if (added.length > 0) note('data', `${name}'s screen reads earlier posts of ${where(room)} from the store: ${added[0].seq} to ${added.at(-1).seq}`);
      return added.length;
    }),
    /**
     * The latest posts of channel `room` as the store holds them, for a
     * screen that has not joined it: each { room, seq, from, text, at }.
     * Reading the store tells no chart, so the room does not know the
     * reader. Null on step 5, whose charts store no posts; a direct
     * message is its two members' only, so none for it.
     */
    preview: (room) => {
      if (current !== LATEST) return null;
      if (directRooms.has(room)) return [];
      return history.read(room).map((post) => ({ room, seq: post.seq, from: post.from, text: post.text, at: post.at }));
    },
    /**
     * Asks the workspace for a room for `by`: answers [event, data],
     * room.created or room.refused. The answer reaches this call only, so
     * the page records here who created the room and when.
     */
    createRoom: (name, by) => act(async () => {
      note('input', `workspace ← room.create #${name}`);
      const answer = await workspace.ask('room.create', { name });
      if (answer[0] === 'room.created') created.set(name, { by, at: now() });
      return answer;
    }),
    /**
     * Who created channel `room` and when (simulated seconds): { by, at },
     * `by` null for the workspace's own channels, made at the start; null
     * when this page did not see it made.
     */
    created: (room) => (ROOMS.includes(room) ? { by: null, at: 0 } : created.get(room) ?? null),
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
    /**
     * Runs scenario `id` (SCENARIOS): its people come online in #general, then
     * its steps go on the agenda. Each does what a person would: signs in
     * unless signed in, and joins #general unless their screen shows it
     * joined (the `joined` the charts sent it), so no room adds a member twice.
     */
    scenario: (id) => {
      // one run of a scenario at a time: a second start while it runs would put every step on
      // the agenda twice. Refused at once, while the first start may still be queued
      if (stepsLeft(id) !== null) return Promise.resolve(false);
      starting.add(id);
      return act(async () => {
        try {
          const scenario = SCENARIOS[id];
          note('scenario', `${scenario.title}: ${scenario.about}`);
          for (const name of ONLINE_FOR[id]) {
            if (!connections.has(name)) open(name);
            if (!screen(name).rooms.includes('general')) tell(name, 'join', { room: 'general' });
          }
          await host.idle();
          for (const [at, step] of scenario.steps) later(at, scenario.title, () => step(direct));
          return true;
        } finally {
          starting.delete(id);
        }
      });
    },
    /**
     * How many of scenario `id`'s steps are still to come, or null when it
     * is not running: while it runs, `scenario(id)` refuses to start it again.
     */
    scenarioLeft: (id) => stepsLeft(id),
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
      // step 5 has no reconnecting: the migration would put a person whose connection is lost in
      // step 5's initial state, offline, running no onentry, so the workspace would never hear of
      // it. Ended first as an application on step 5 would have: signed out now, and back at the
      // reconnect the agenda holds (step 5's connect)
      if (step !== LATEST) {
        for (const [name, entry] of losses) {
          tell(name, 'disconnect');
          note('version', `${name}'s connection is lost; step ${step} has no reconnecting, so ${name} is signed out until the reconnect at ${minutes(entry.at / 1000)}`);
        }
        // step 5's workspace keeps no direct messages, and its room chart no `direct`: back on
        // step 9 the workspace would make each again and its room would let anyone join. So each
        // ends here, its posts kept in the store, and the workspace makes it again on the way back
        for (const room of directRooms.keys()) {
          host.actor(`room.${room}`).terminate();
          note('version', `step ${step} has no direct messages: ${room} ends, its posts stay in the store`);
        }
        await host.idle();
      }
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
      // back on step 9 the workspace makes each direct message again, asked for one of its members;
      // its members' screens read its posts from the store when they open it
      if (step === LATEST) {
        for (const [room, [user, other]] of directRooms) {
          await workspace.ask('dm.open', { user, with: other });
          note('version', `the workspace makes ${room} again`);
        }
      }
      note('version', `moved to ${VERSIONS[step].split(':')[0]}: ${result?.migrated ?? 0} actors migrated, ${result?.failed?.length ?? 0} failed`);
      return result;
    }),

    /** Every actor by name, with its chart, whether it is a direct message's room, and whether it is in memory or saved in the store. */
    actors: () => read(async () => {
      const direct = current === LATEST ? [...directRooms.keys()].sort().map((room) => `room.${room}`) : [];
      const names = ['workspace', ...(await workspace.ask('rooms.list'))[1].rooms.map((room) => `room.${room}`), ...direct, ...PEOPLE.map((name) => `user.${name}`)];
      return names.map((name) => {
        const actor = host.actor(name);
        return { name, chart: name.split('.')[0], direct: direct.includes(name), session: actor.id, stored: store.blobs.has(String(actor.id)) };
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
