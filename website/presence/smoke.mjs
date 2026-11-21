// The presence server, run here against two tabs: it starts server.mjs on
// a free port, and checks what a browser relies on.
//   - the Origin check and the ticket (a forged one and a reused one refused);
//   - welcome, then trips for both ants;
//   - a greeting reaches the other tab as its country;
//   - a duplicated tab (the same tab id while connected) is refused;
//   - a reload within the grace gets the same ant back;
//   - /health counts them.
// Prints one line per check; exits 1 on the first that fails.
//   node smoke.mjs                       (against a server it starts)
//   node smoke.mjs wss://host [origin]   (against a running one: TICKET_KEY must match it)
import { spawn } from 'node:child_process';
import { WebSocket } from 'ws';
import { issue } from './ticket.mjs';

const KEY = process.env.TICKET_KEY ?? 'smoke-test-key';
const ORIGIN = process.argv[3] ?? 'https://www.tinyactors.dev';
let url = process.argv[2], server;
if (!url) {
  server = spawn(process.execPath, [new URL('./server.mjs', import.meta.url).pathname], { env: { ...process.env, PORT: '0', TICKET_KEY: KEY, ORIGINS: ORIGIN }, stdio: ['ignore', 'pipe', 'inherit'] });
  url = await new Promise((resolve, reject) => {
    server.stdout.on('data', (data) => { const m = /listening on (\d+)/.exec(String(data)); if (m) resolve(`ws://127.0.0.1:${m[1]}`); });
    server.on('exit', (code) => reject(new Error(`server exited ${code}`)));
  });
}
const ok = (what) => console.log(`ok: ${what}`);
const fail = (what) => { console.log(`FAILED: ${what}`); server?.kill(); process.exit(1); };
const tab = () => 'smoke' + Math.random().toString(36).slice(2, 12);

function open(origin = ORIGIN) {
  const socket = new WebSocket(url, { origin });
  const inbox = [];
  const waiters = [];
  socket.on('message', (raw) => {
    const message = JSON.parse(String(raw));
    const messages = message.t === 'batch' ? message.m : [message];
    for (const m of messages) { inbox.push(m); for (const w of [...waiters]) if (w.test(m)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(m); } }
  });
  const closed = new Promise((resolve) => socket.on('close', (code) => resolve(code)));
  const opened = new Promise((resolve, reject) => { socket.on('open', resolve); socket.on('unexpected-response', (_, response) => reject(response.statusCode)); socket.on('error', () => {}); });
  return {
    socket, closed, opened, inbox,
    send: (m) => socket.send(JSON.stringify(m)),
    wait: (test, ms = 15_000) => {
      const found = inbox.find(test);
      if (found) return Promise.resolve(found);
      return new Promise((resolve, reject) => { const w = { test, resolve }; waiters.push(w); setTimeout(() => reject(new Error('timed out')), ms); });
    },
  };
}

// The Origin check
const stranger = open('https://example.com');
const status = await stranger.opened.then(() => 'opened', (code) => code);
status === 403 ? ok('a page of another origin is refused (403)') : fail(`another origin: ${status}`);

// A forged ticket, then a good one used twice
const forged = open();
await forged.opened;
forged.send({ t: 'hello', ticket: await issue('not-the-key', 'EE'), tab: tab() });
(await forged.closed) === 4001 ? ok('a ticket signed with another key is refused') : fail('forged ticket');

const ticketA = await issue(KEY, 'EE');
const a = open(); await a.opened;
const tabA = tab();
a.send({ t: 'hello', ticket: ticketA, tab: tabA });
const welcomeA = await a.wait((m) => m.t === 'welcome');
ok(`tab A welcomed as ant ${welcomeA.you}, from EE`);
const replay = open(); await replay.opened;
replay.send({ t: 'hello', ticket: ticketA, tab: tab() });
(await replay.closed) === 4001 ? ok('a ticket used once is refused the second time') : fail('ticket replay');

const b = open(); await b.opened;
b.send({ t: 'hello', ticket: await issue(KEY, 'DE'), tab: tab() });
const welcomeB = await b.wait((m) => m.t === 'welcome');
welcomeB.ants.some(([h]) => h === welcomeA.you) ? ok(`tab B welcomed as ant ${welcomeB.you}, and sees ant ${welcomeA.you}`) : fail('B does not see A');
await a.wait((m) => m.t === 'trip' && m.trip[0] === welcomeB.you);
ok('tab A gets the trips of ant B');

// A greeting from A to B
a.send({ t: 'greet', h: welcomeB.you });
const greeted = await b.wait((m) => m.t === 'greeted', 30_000);
greeted.country === 'EE' ? ok('B is told: an ant from EE greets you') : fail(`greeted: ${JSON.stringify(greeted)}`);
await a.wait((m) => m.t === 'greet' && m.a === welcomeA.you && m.b === welcomeB.you);
ok('A sees the two ants touch antennae');

// A duplicated tab
const dup = open(); await dup.opened;
dup.send({ t: 'hello', ticket: await issue(KEY, 'EE'), tab: tabA });
(await dup.closed) === 4009 ? ok('a second connection with a connected tab id is refused') : fail('duplicate tab');

// A reload within the grace
a.socket.close();
await a.closed;
await new Promise((resolve) => setTimeout(resolve, 300));
const again = open(); await again.opened;
again.send({ t: 'hello', ticket: await issue(KEY, 'EE'), tab: tabA });
const welcomeAgain = await again.wait((m) => m.t === 'welcome');
welcomeAgain.you === welcomeA.you ? ok(`a reload gets its ant back (ant ${welcomeAgain.you})`) : fail('reload');

// Health
const health = await (await fetch(url.replace(/^ws/, 'http') + '/health')).json();
health.connected === 2 && health.ants === 2 ? ok('/health: 2 tabs connected, 2 ants') : fail(`health: ${JSON.stringify(health)}`);

again.send({ t: 'leave' });
b.send({ t: 'leave' });
await Promise.all([again.closed, b.closed]);
server?.kill();
process.exit(0);
