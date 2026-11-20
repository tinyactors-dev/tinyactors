// Step 3: the workspace decides which rooms exist. Run: node main.mjs
import { compileDir, Host, Spawner } from '@tinyactors/node';

class Client {
  lines = [];

  receive(send) {
    const { data } = send;
    if (send.event === 'joined') this.lines.push(`${data.to} joined ${data.room}`);
    if (send.event === 'error') this.lines.push(`${data.to} gets an error: ${data.reason}`);
    if (send.event === 'message') this.lines.push(`${data.to} sees ${data.from} in ${data.room}: ${data.text}`);
  }
}

export async function run(options = {}) {
  const client = new Client();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, {
    system: 'chat',
    ioProcessors: { 'chat:client': client, spawn: new Spawner() },
    ...options,
  });
  compiled.close();

  const workspace = host.spawn('workspace', { name: 'workspace' });
  for (const name of ['ada', 'bob']) host.spawn('user', { name: `user.${name}`, params: { name } }).send('connect');
  const ada = host.actor('user.ada');
  const bob = host.actor('user.bob');

  for (const name of ['general', 'general', 'Bad Name']) {
    const [event, data] = await workspace.ask('room.create', { name });
    client.lines.push(`create ${name}: ${event}${data.reason ? ` (${data.reason})` : ''}`);
  }
  const [, { rooms }] = await workspace.ask('rooms.list');
  client.lines.push(`rooms: ${rooms.join(', ')}`);

  ada.send('join', { room: 'general' });
  bob.send('join', { room: 'general' });
  bob.send('join', { room: 'random' });
  await host.idle();

  ada.send('post', { room: 'general', text: 'hi' });
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
