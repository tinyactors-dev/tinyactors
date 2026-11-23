// The team chat of the tutorial (examples/chat), finished: step 5's charts,
// a workspace that makes rooms, users who come and go, and a store that
// keeps idle actors out of memory. It takes the tinyactors package as an
// argument, so the same code runs in Node.js (main.mjs, against
// output.txt) and in the reader's browser (page.mjs, on @tinyactors/wasm).
//
// What each user's screen shows is built only from what the charts send to
// `chat:client` for that user: the messages, the roster and its changes,
// the rooms joined and the errors. The page draws it; nothing here decides
// what a user sees.

// The people of the workspace, The Hill: an ant colony's castes, each a
// different initial for the page's avatars.
export const WORKSPACE = 'The Hill';
export const PEOPLE = ['queen', 'scout', 'forager', 'nurse', 'guard', 'drone'];
export const ROOMS = ['general', 'random'];
export const RESIDENT = 4; // actors the host keeps in memory at rest
const KEEP = 200; // the newest messages a screen keeps

/** A store that keeps hibernated actors' blobs in a Map, so the page can say which are saved. */
class Store {
  blobs = new Map();
  save(session, blob) { this.blobs.set(String(session), blob); }
  load(session) { return this.blobs.get(String(session)); }
  delete(session) { this.blobs.delete(String(session)); }
}

/** Each user's screen, built from what the charts send to chat:client. */
class Screens {
  byUser = new Map();

  screen(user) {
    let screen = this.byUser.get(user);
    if (!screen) this.byUser.set(user, (screen = { rooms: [], messages: [], roster: {}, notices: [] }));
    return screen;
  }

  receive(send) {
    const { data } = send;
    const screen = this.screen(data.to);
    if (send.event === 'message') {
      screen.messages.push({ room: data.room, from: data.from, text: data.text });
      if (screen.messages.length > KEEP) screen.messages.shift();
    } else if (send.event === 'roster') screen.roster = { ...data.roster };
    else if (send.event === 'roster.update') screen.roster = { ...screen.roster, [data.user]: data.status };
    else if (send.event === 'joined') { if (!screen.rooms.includes(data.room)) screen.rooms = [...screen.rooms, data.room]; }
    else if (send.event === 'error') screen.notices.push(data.reason);
  }
}

/**
 * The workspace, its rooms and PEOPLE, on a host with a simulated clock.
 * `charts` maps each chart's file name to its text. Every action waits for
 * the host to settle, one at a time, and then calls `onChange`.
 */
export async function createChat(tinyactors, { charts, onChange = () => {} } = {}) {
  const { compileCharts, Host, SimulatedClock, Spawner } = tinyactors;
  const screens = new Screens();
  const store = new Store();
  const clock = new SimulatedClock();
  const compiled = compileCharts(Object.entries(charts).map(([name, text]) => ({ name, text })));
  const host = new Host(compiled, {
    system: 'chat',
    clock,
    store,
    resident: RESIDENT,
    ioProcessors: { 'chat:client': screens, spawn: new Spawner() },
  });
  compiled.close();

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

  const workspace = host.spawn('workspace', { name: 'workspace' });
  for (const name of PEOPLE) host.spawn('user', { name: `user.${name}`, params: { name } });
  await host.idle();
  for (const room of ROOMS) await workspace.ask('room.create', { name: room });
  await host.idle();

  const user = (name) => host.actor(`user.${name}`);

  return {
    /** What `user`'s screen shows: the rooms they joined, their messages, the roster, the errors. */
    screen: (name) => screens.screen(name),
    /** The simulated time, in seconds. */
    now: () => clock.now / 1000,
    connect: (name) => act(() => user(name).send('connect')),
    disconnect: (name) => act(() => user(name).send('disconnect')),
    /** Typing: the user's away timer starts again. */
    activity: (name) => act(() => user(name).send('activity')),
    join: (name, room) => act(() => user(name).send('join', { room })),
    post: (name, room, text) => act(() => user(name).send('post', { room, text })),
    /** Asks the workspace for a room: answers [event, data], room.created or room.refused. */
    createRoom: (name) => act(() => workspace.ask('room.create', { name })),
    /** The rooms the workspace holds. */
    rooms: () => read(async () => (await workspace.ask('rooms.list'))[1].rooms),
    /** Moves the simulated clock `ms` on: due timers fire. */
    advance: (ms) => act(() => clock.increment(ms)),
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
}
