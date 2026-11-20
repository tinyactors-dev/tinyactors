// Step 2: users with presence. Run: node main.mjs
import { compileDir, Host, SimulatedClock } from '@tinyactors/node';

class Client {
  lines = [];

  receive(send) {
    if (send.event === 'status') this.lines.push(`${send.data.user} is ${send.data.status}`);
    if (send.event === 'message') this.lines.push(`${send.data.to} sees ${send.data.from}: ${send.data.text}`);
  }
}

export async function run(options = {}) {
  const client = new Client();
  const clock = new SimulatedClock();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, { system: 'chat', clock, ioProcessors: { 'chat:client': client }, ...options });
  compiled.close();

  const room = host.spawn('room', { name: 'room.general' });
  for (const name of ['ada', 'bob']) {
    host.spawn('user', { name: `user.${name}`, params: { name } }).send('connect');
    room.send('join', { user: name });
  }
  await host.idle();

  client.lines.push('-- 300 seconds pass');
  clock.increment(300_000);
  await host.idle();

  client.lines.push('-- bob types, ada closes the app');
  host.actor('user.bob').send('activity');
  host.actor('user.ada').send('disconnect');
  room.send('post', { user: 'bob', text: 'anyone?' });
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
