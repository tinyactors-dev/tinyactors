// Step 8: a room's history. Run: node main.mjs
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
// once: one the join read from the store may reach it as a message too.
// When a user's connection opens, their screen reads the posts it lacks.
class Client {
  lines = [];
  shown = new Set(); // "user room seq"
  rooms = new Map(); // user -> the rooms their screen joined

  constructor(history) {
    this.history = history;
  }

  receive(send) {
    const { data } = send;
    if (send.event === 'joined') {
      this.rooms.set(data.to, new Set(this.rooms.get(data.to)).add(data.room));
      const posts = this.history.read(data.room);
      for (const post of posts) this.shown.add(`${data.to} ${data.room} ${post.seq}`);
      const text = posts.map(said).join(' | ') || 'nothing yet';
      this.lines.push(`${data.to} joins ${data.room} and reads ${text}`);
    }
    if (send.event === 'message') {
      const key = `${data.to} ${data.room} ${data.seq}`;
      if (this.shown.has(key)) return;
      this.shown.add(key);
      this.lines.push(`${data.to} sees post ${data.seq} from ${said(data)}`);
    }
  }

  /** `user`'s connection opens: for each room joined, the screen reads the posts it lacks. */
  opened(user) {
    for (const room of this.rooms.get(user) ?? []) {
      const posts = this.history.read(room).filter((post) => !this.shown.has(`${user} ${room} ${post.seq}`));
      if (posts.length === 0) continue;
      for (const post of posts) this.shown.add(`${user} ${room} ${post.seq}`);
      this.lines.push(`${user} connects and reads in ${room}: ${posts.map(said).join(' | ')}`);
    }
  }
}

export async function run({ clock = new SimulatedClock(), history = new History(clock), ...options } = {}) {
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
  for (const name of ['ada', 'bob', 'cat']) host.spawn('user', { name: `user.${name}`, params: { name } }).send('connect');
  await host.idle();
  const [ada, bob, cat] = ['ada', 'bob', 'cat'].map((name) => host.actor(`user.${name}`));

  client.lines.push('-- ada and bob join and talk');
  ada.send('join', { room: 'general' });
  bob.send('join', { room: 'general' });
  await host.idle();
  ada.send('post', { room: 'general', text: 'hi bob' });
  await host.idle();
  clock.increment(30_000);
  bob.send('post', { room: 'general', text: 'hi ada' });
  await host.idle();

  clock.increment(90_000);
  client.lines.push('-- cat joins late, bob joins again');
  cat.send('join', { room: 'general' });
  bob.send('join', { room: 'general' });
  await host.idle();
  ada.send('post', { room: 'general', text: 'welcome, cat' });
  await host.idle();

  client.lines.push('-- cat signs out, and back in five minutes later');
  cat.send('disconnect');
  await host.idle();
  clock.increment(60_000);
  bob.send('post', { room: 'general', text: 'lunch at noon' });
  await host.idle();
  clock.increment(300_000);
  client.opened('cat'); // cat's connection opens: the screen catches up,
  cat.send('connect'); // then cat's actor hears of it
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
