// Step 4: the status roster. Run: node main.mjs
import { compileDir, Host, SimulatedClock, Spawner } from '@tinyactors/node';

class Client {
  lines = [];

  receive(send) {
    const { data } = send;
    if (send.event === 'roster') {
      const entries = Object.entries(data.roster).map(([user, status]) => `${user} ${status}`);
      this.lines.push(`${data.to} sees the roster: ${entries.join(', ')}`);
    }
    if (send.event === 'roster.update') this.lines.push(`${data.to} sees ${data.user} is ${data.status}`);
  }
}

export async function run(options = {}) {
  const client = new Client();
  const clock = new SimulatedClock();
  const compiled = compileDir(new URL('./charts/', import.meta.url));
  const host = new Host(compiled, {
    system: 'chat',
    clock,
    ioProcessors: { 'chat:client': client, spawn: new Spawner() },
    ...options,
  });
  compiled.close();

  const workspace = host.spawn('workspace', { name: 'workspace' });
  for (const name of ['ada', 'bob', 'cat']) host.spawn('user', { name: `user.${name}`, params: { name } });
  await host.idle();

  client.lines.push('-- ada connects');
  host.actor('user.ada').send('connect');
  await host.idle();

  client.lines.push('-- bob connects');
  host.actor('user.bob').send('connect');
  await host.idle();

  client.lines.push('-- 300 seconds pass');
  clock.increment(300_000);
  await host.idle();

  client.lines.push('-- bob closes the app');
  host.actor('user.bob').send('disconnect');
  await host.idle();

  const [, { roster }] = await workspace.ask('roster.get');
  client.lines.push(`the workspace answers: ${JSON.stringify(roster)}`);

  host.close();
  return client.lines;
}

if (import.meta.main ?? process.argv[1] === new URL(import.meta.url).pathname) {
  for (const line of await run()) console.log(line);
}
