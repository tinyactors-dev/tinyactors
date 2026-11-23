// Step 8: a room's history. Run: node main.mjs
import { compileDir, Host, MemoryStore, SimulatedClock, Spawner } from '@tinyactors/node';

// The channels' history: data the application stores, one list of posts a
// channel. The room chart appends each post (chat:history); a screen reads
// a channel's posts up to the number its join brought. A real one is a
// table in a database.
class History {
  posts = new Map(); // room -> [{ seq, from, text }]

  receive(send) {
    const { room, seq, from, text } = send.data;
    if (!this.posts.has(room)) this.posts.set(room, []);
    this.posts.get(room).push({ seq, from, text });
  }

  /** The posts of `room` numbered up to `seq`, the last `limit` of them. */
  read(room, seq, limit = 50) {
    return (this.posts.get(room) ?? []).filter((post) => post.seq <= seq).slice(-limit);
  }
}

class Client {
  lines = [];

  constructor(history) {
    this.history = history;
  }

  receive(send) {
    const { data } = send;
    if (send.event === 'joined') {
      const posts = this.history.read(data.room, data.seq).map((post) => `${post.from}: ${post.text}`).join(' | ') || 'nothing yet';
      this.lines.push(`${data.to} joins ${data.room} and reads ${posts}`);
    }
    if (send.event === 'message') this.lines.push(`${data.to} sees post ${data.seq} from ${data.from}: ${data.text}`);
  }
}

export async function run(options = {}) {
  const history = new History();
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

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
