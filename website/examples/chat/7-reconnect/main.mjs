// Step 7: ride out a lost connection. Run: node main.mjs
import { compileDir, Host, MemoryStore, SimulatedClock, Spawner } from '@tinyactors/node';

class Client {
  lines = [];

  receive(send) {
    const { data } = send;
    if (send.event === 'roster.update') this.lines.push(`${data.to} sees ${data.user} is ${data.status}`);
    if (send.event === 'roster') {
      const statuses = Object.keys(data.roster).sort().map((user) => `${user} ${data.roster[user]}`);
      this.lines.push(`${data.to} sees the roster: ${statuses.join(', ')}`);
    }
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
  for (const name of ['ada', 'bob', 'cy']) {
    const user = host.spawn('user', { name: `user.${name}`, params: { name } });
    user.send('connect');
    user.send('join', { room: 'general' });
  }
  await host.idle();
  client.lines.length = 0; // the roster as they came online: step 4's
  const ada = host.actor('user.ada');
  const bob = host.actor('user.bob');
  const cy = host.actor('user.cy');

  client.lines.push('-- bob enters a tunnel, cy signs out, ada posts');
  bob.send('connection.lost');
  await host.idle();
  cy.send('disconnect');
  await host.idle();
  ada.send('post', { room: 'general', text: 'where are you?' });
  await host.idle();

  client.lines.push('-- 10 seconds later bob is back');
  clock.increment(10_000);
  bob.send('connect');
  await host.idle();

  client.lines.push('-- bob loses the connection for 40 seconds');
  bob.send('connection.lost');
  await host.idle();
  clock.increment(40_000);
  await host.idle();
  ada.send('post', { room: 'general', text: 'gone?' });
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
