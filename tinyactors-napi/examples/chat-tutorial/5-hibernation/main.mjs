// Step 5: many users, few in memory. Run: node main.mjs
import { compileDir, Host, SimulatedClock, Spawner } from '@tinyactors/node';

// A store keeps the blobs of hibernated actors. This one keeps them in a
// Map and counts its calls. A real one writes to a database or to files.
class Store {
  blobs = new Map();
  saves = 0;
  loads = 0;

  save(session, blob) {
    this.saves++;
    this.blobs.set(String(session), blob);
  }

  load(session) {
    this.loads++;
    return this.blobs.get(String(session));
  }

  delete(session) {
    this.blobs.delete(String(session));
  }
}

class Client {
  messages = 0;
  updates = 0;

  receive(send) {
    if (send.event === 'message') this.messages++;
    if (send.event === 'roster.update') this.updates++;
  }
}

export async function run({ users = 1000, online = 100, resident = 50, ...options } = {}) {
  const client = new Client();
  const store = new Store();
  const clock = new SimulatedClock();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, {
    system: 'chat',
    clock,
    store,
    resident,
    ioProcessors: { 'chat:client': client, spawn: new Spawner() },
    ...options,
  });
  compiled.close();

  const lines = [];
  let highest = 0;
  async function report(what) {
    await host.idle();
    const { host: stats } = await host.hostStats();
    const inMemory = stats.actors - stats.hibernated;
    highest = Math.max(highest, inMemory);
    lines.push(`${what}: ${stats.actors} actors, ${inMemory} in memory, ${store.blobs.size} in the store`);
  }

  const workspace = host.spawn('workspace', { name: 'workspace' });
  await workspace.ask('room.create', { name: 'general' });
  for (let n = 0; n < users; n++) host.spawn('user', { name: `user.u${n}`, params: { name: `u${n}` } });
  await report(`${users} users spawned`);

  for (let n = 0; n < online; n++) {
    const user = host.actor(`user.u${n}`);
    user.send('connect');
    user.send('join', { room: 'general' });
  }
  await report(`${online} users connect and join general`);

  host.actor('user.u0').send('post', { room: 'general', text: 'hi' });
  await report('u0 posts');
  lines.push(`${client.messages} messages reached the screens`);

  clock.increment(300_000);
  await report('300 seconds pass');
  lines.push(`${client.updates} roster updates reached the screens`);

  lines.push(`the store saved ${store.saves} blobs and loaded ${store.loads}`);
  lines.push(`at most ${highest} actors were in memory at rest`);
  host.close();
  return lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
