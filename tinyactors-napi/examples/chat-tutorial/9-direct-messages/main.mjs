// Step 9: direct messages. Run: node main.mjs
import { compileDir, Host, MemoryStore, SimulatedClock, Spawner } from '@tinyactors/node';

// The channels' history: data the application stores, one list of posts a
// channel. The room chart sends each post here (chat:history); the store
// numbers it, records when it was made by the clock it is given, keeps it
// and answers `appended` with both, which the room puts on the post it
// sends each member. The numbers count per channel, as a channel's posts
// are read together, and never start again: the store, not an actor, holds
// the count. A screen shows a post's own time, however late the post
// reaches it, and reads a channel's latest posts when it joins. A real
// store is a table in a database.
export class History {
  posts = new Map(); // room -> [{ seq, at, from, text }]

  constructor(clock) {
    this.clock = clock;
  }

  receive(send) {
    const { room, from, text } = send.data;
    if (!this.posts.has(room)) this.posts.set(room, []);
    const posts = this.posts.get(room);
    const seq = posts.length + 1;
    const at = this.clock.now; // milliseconds
    posts.push({ seq, at, from, text });
    send.answer('appended', { seq, at, room, from, text });
  }

  /** The latest `limit` posts of `room`. */
  read(room, limit = 50) {
    return (this.posts.get(room) ?? []).slice(-limit);
  }
}

// A post as a screen shows it: its author, its time (minutes:seconds) and its text.
const said = ({ at, from, text }) =>
  `${from} at ${Math.floor(at / 60_000)}:${String(Math.floor(at / 1000) % 60).padStart(2, '0')}: ${text}`;

// What reaches each user's screen, as lines. A screen shows a numbered post
// once: one read from the store may reach it as a message too. It knows
// the direct messages the charts told it of, each by the other person.
// When a user's connection opens, their screen reads the posts it lacks.
class Client {
  lines = [];
  shown = new Set(); // "user room seq"
  rooms = new Map(); // user -> the rooms their screen joined, direct messages included
  dms = new Map(); // user -> (room -> the other person)

  constructor(history) {
    this.history = history;
  }

  /** Where `room` is, as `user`'s screen names it. */
  place(user, room) {
    const other = this.dms.get(user)?.get(room);
    return other === undefined ? room : `the DM with ${other}`;
  }

  /** `user`'s screen joins `room`: it reads the latest posts from the store. */
  read(user, room) {
    this.rooms.set(user, new Set(this.rooms.get(user)).add(room));
    const posts = this.history.read(room);
    for (const post of posts) this.shown.add(`${user} ${room} ${post.seq}`);
    return posts.map(said).join(' | ') || 'nothing yet';
  }

  /** `user`'s screen learns of the direct message `room` with `other`. */
  direct(user, room, other) {
    if (!this.dms.has(user)) this.dms.set(user, new Map());
    this.dms.get(user).set(room, other);
  }

  receive(send) {
    const { data } = send;
    if (send.event === 'joined') this.lines.push(`${data.to} joins ${data.room} and reads ${this.read(data.to, data.room)}`);
    if (send.event === 'dm.opened') {
      this.direct(data.to, data.room, data.with);
      this.lines.push(`${data.to} opens ${data.room}, the DM with ${data.with}, and reads ${this.read(data.to, data.room)}`);
    }
    if (send.event === 'dms') {
      for (const dm of data.dms) {
        if (this.dms.get(data.to)?.has(dm.room)) continue;
        this.direct(data.to, dm.room, dm.with);
        this.lines.push(`${data.to} has ${dm.room}, the DM with ${dm.with}, and reads ${this.read(data.to, dm.room)}`);
      }
    }
    if (send.event === 'message') {
      const key = `${data.to} ${data.room} ${data.seq}`;
      if (this.shown.has(key)) return;
      this.shown.add(key);
      this.lines.push(`${data.to} sees post ${data.seq} in ${this.place(data.to, data.room)} from ${said(data)}`);
    }
    if (send.event === 'typing') this.lines.push(`${data.to} sees ${data.user} typing in ${this.place(data.to, data.room)}`);
    if (send.event === 'error') this.lines.push(`${data.to} gets an error: ${data.reason}`);
  }

  /** `user`'s connection opens: for each room joined, the screen reads the posts it lacks. */
  opened(user) {
    for (const room of this.rooms.get(user) ?? []) {
      const posts = this.history.read(room).filter((post) => !this.shown.has(`${user} ${room} ${post.seq}`));
      if (posts.length === 0) continue;
      for (const post of posts) this.shown.add(`${user} ${room} ${post.seq}`);
      this.lines.push(`${user} connects and reads in ${this.place(user, room)}: ${posts.map(said).join(' | ')}`);
    }
  }
}

/** The chat's host, with a workspace and the channel general; no user yet. */
export async function start({ clock = new SimulatedClock(), history = new History(clock), ...options } = {}) {
  const client = new Client(history);
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, {
    system: 'chat',
    clock,
    store: new MemoryStore(),
    resident: 50,
    ioProcessors: { 'chat:client': client, 'chat:history': history, spawn: new Spawner() },
    ...options,
  });
  compiled.close();

  const workspace = host.spawn('workspace', { name: 'workspace' });
  await workspace.ask('room.create', { name: 'general' });
  return { host, client, clock, workspace };
}

export async function run(options) {
  const { host, client, clock, workspace } = await start(options);
  for (const name of ['scout', 'forager', 'queen']) host.spawn('user', { name: `user.${name}`, params: { name } });
  const [scout, forager, queen] = ['scout', 'forager', 'queen'].map((name) => host.actor(`user.${name}`));
  scout.send('connect');
  queen.send('connect');
  await host.idle();

  client.lines.push('-- scout opens a DM with forager, who is signed out, and posts twice');
  scout.send('dm.open', { with: 'forager' });
  await host.idle();
  const dm = [...client.dms.get('scout').keys()][0];
  scout.send('post', { room: dm, text: 'sugar by the oak' });
  await host.idle();
  clock.increment(30_000);
  scout.send('post', { room: dm, text: 'can you carry?' });
  await host.idle();

  clock.increment(30_000);
  client.lines.push('-- forager signs in, types and answers');
  client.opened('forager');
  forager.send('connect');
  await host.idle();
  forager.send('typing', { room: dm });
  await host.idle();
  forager.send('post', { room: dm, text: 'on my way' });
  await host.idle();

  client.lines.push('-- forager opens the DM with scout: the same one');
  forager.send('dm.open', { with: 'scout' });
  await host.idle();

  client.lines.push('-- queen tries to read it');
  queen.send('join', { room: dm });
  queen.send('post', { room: dm, text: 'let me in' });
  await host.idle();
  const [, { rooms }] = await workspace.ask('rooms.list');
  client.lines.push(`the channels: ${rooms.join(', ')}`);

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
