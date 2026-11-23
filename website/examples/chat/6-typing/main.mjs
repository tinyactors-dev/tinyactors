// Step 6: who is typing. Run: node main.mjs
import { compileDir, Host, MemoryStore, SimulatedClock, Spawner } from '@tinyactors/node';

class Client {
  lines = [];

  receive(send) {
    const { data } = send;
    if (send.event === 'typing') this.lines.push(`${data.to} sees ${data.user} typing in ${data.room}`);
    if (send.event === 'typing.stopped') this.lines.push(`${data.to} sees ${data.user} stop typing in ${data.room}`);
    if (send.event === 'message') this.lines.push(`${data.to} sees ${data.from} in ${data.room}: ${data.text}`);
  }
}

export async function run(options = {}) {
  const client = new Client();
  const clock = new SimulatedClock();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, {
    system: 'chat',
    clock,
    store: new MemoryStore(),
    resident: 50,
    ioProcessors: { 'chat:client': client, spawn: new Spawner() },
    ...options,
  });
  compiled.close();

  const workspace = host.spawn('workspace', { name: 'workspace' });
  await workspace.ask('room.create', { name: 'general' });
  for (const name of ['ada', 'bob', 'cat']) {
    const user = host.spawn('user', { name: `user.${name}`, params: { name } });
    user.send('connect');
    user.send('join', { room: 'general' });
  }
  await host.idle();
  const ada = host.actor('user.ada');
  const bob = host.actor('user.bob');

  client.lines.push('-- ada types three keys');
  for (let key = 0; key < 3; key++) ada.send('typing', { room: 'general' });
  await host.idle();

  client.lines.push('-- 4 seconds pass, ada types and posts');
  clock.increment(4_000);
  ada.send('typing', { room: 'general' });
  await host.idle();
  ada.send('post', { room: 'general', text: 'lunch?' });
  await host.idle();

  client.lines.push('-- bob types, then stops for 6 seconds');
  bob.send('typing', { room: 'general' });
  await host.idle();
  clock.increment(6_000);
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
