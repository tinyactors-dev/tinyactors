// Step 1: a room. Run: node main.mjs
import { compileDir, Host } from '@tinyactors/node';

// The client processor stands in for the users' screens.
class Client {
  lines = [];

  receive(send) {
    for (const user of send.data.to) this.lines.push(`${user} sees ${send.data.from}: ${send.data.text}`);
  }
}

export async function run(options = {}) {
  const client = new Client();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, { system: 'chat', ioProcessors: { 'chat:client': client }, ...options });
  compiled.close();

  const room = host.spawn('room', { name: 'general' });
  room.send('join', { user: 'ada' });
  room.send('join', { user: 'bob' });
  room.send('post', { user: 'ada', text: 'hi' });
  await host.idle();

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
