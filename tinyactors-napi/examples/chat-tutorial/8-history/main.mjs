// Step 8: a room's history. Run: node main.mjs
import { compileDir, Host, MemoryStore, SimulatedClock, Spawner } from '@tinyactors/node';

// The channels' history: data the application stores, one list of posts a
// channel. The room chart sends each post here (chat:history); the store
// numbers it, keeps it and answers `appended` with the number, which the
// room puts on the post it sends each member. The numbers count per
// channel, as a channel's posts are read together, and never start again:
// the store, not an actor, holds the count. A screen reads a channel's
// latest posts when it joins. A real store is a table in a database.
export class History {
  posts = new Map(); // room -> [{ seq, from, text }]

  receive(send) {
    const { room, from, text } = send.data;
    if (!this.posts.has(room)) this.posts.set(room, []);
    const posts = this.posts.get(room);
    const seq = posts.length + 1;
    posts.push({ seq, from, text });
    send.answer('appended', { seq, room, from, text });
  }

  /** The latest `limit` posts of `room`. */
  read(room, limit = 50) {
    return (this.posts.get(room) ?? []).slice(-limit);
  }
}

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
      const text = posts.map((post) => `${post.from}: ${post.text}`).join(' | ') || 'nothing yet';
      this.lines.push(`${data.to} joins ${data.room} and reads ${text}`);
    }
    if (send.event === 'message') {
      const key = `${data.to} ${data.room} ${data.seq}`;
      if (this.shown.has(key)) return;
      this.shown.add(key);
      this.lines.push(`${data.to} sees post ${data.seq} from ${data.from}: ${data.text}`);
    }
  }

  /** `user`'s connection opens: for each room joined, the screen reads the posts it lacks. */
  opened(user) {
    for (const room of this.rooms.get(user) ?? []) {
      const posts = this.history.read(room).filter((post) => !this.shown.has(`${user} ${room} ${post.seq}`));
      if (posts.length === 0) continue;
      for (const post of posts) this.shown.add(`${user} ${room} ${post.seq}`);
      this.lines.push(`${user} connects and reads in ${room}: ${posts.map((post) => `${post.from}: ${post.text}`).join(' | ')}`);
    }
  }
}

export async function run({ history = new History(), ...options } = {}) {
  const client = new Client(history);
  const clock = new SimulatedClock();
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
  bob.send('post', { room: 'general', text: 'hi ada' });
  await host.idle();

  client.lines.push('-- cat joins late, bob joins again');
  cat.send('join', { room: 'general' });
  bob.send('join', { room: 'general' });
  await host.idle();
  ada.send('post', { room: 'general', text: 'welcome, cat' });
  await host.idle();

  client.lines.push('-- cat signs out, and back in');
  cat.send('disconnect');
  await host.idle();
  bob.send('post', { room: 'general', text: 'lunch at noon' });
  await host.idle();
  client.opened('cat'); // cat's connection opens: the screen catches up,
  cat.send('connect'); // then cat's actor hears of it
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
