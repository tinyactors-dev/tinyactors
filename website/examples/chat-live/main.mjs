// The live chat, run in Node.js: what the page's buttons do, in order,
// printed as each screen shows it. Run: node main.mjs
import * as tinyactors from '@tinyactors/wasm';
import { readFileSync } from 'node:fs';
import { createChat } from './chat.mjs';

const charts = Object.fromEntries(['workspace.scxml', 'room.scxml', 'user.scxml']
  .map((name) => [name, readFileSync(new URL(`./charts/${name}`, import.meta.url), 'utf8')]));
const chat = await createChat(tinyactors, { charts });
const show = async (what) => {
  const { actors, inMemory, stored } = await chat.stats();
  console.log(`${what}: ${actors} actors, ${inMemory} in memory, ${stored} in the store`);
};

await show('6 people, 2 rooms');
await chat.connect('ada');
await chat.connect('bob');
await chat.join('ada', 'general');
await chat.join('bob', 'general');
await chat.post('ada', 'general', 'hi bob');
await show('ada and bob connect, join general and talk');
for (const name of ['ada', 'bob']) {
  const screen = chat.screen(name);
  console.log(`${name} sees: ${screen.messages.map((m) => `${m.from} in ${m.room}: ${m.text}`).join(' | ')}`);
  console.log(`${name}'s roster: ${Object.entries(screen.roster).map(([who, status]) => `${who} ${status}`).join(', ')}`);
}
const [event, data] = await chat.createRoom('general');
console.log(`create general again: ${event} (${data.reason})`);
await chat.join('ada', 'design');
console.log(`ada joins design: ${chat.screen('ada').notices.at(-1)}`);
await chat.advance(300_000);
await show('300 seconds pass');
console.log(`bob's roster: ${Object.entries(chat.screen('bob').roster).map(([who, status]) => `${who} ${status}`).join(', ')}`);
for (const name of ['workspace', 'room.general', 'user.ada', 'user.cat']) {
  const state = await chat.state(name);
  console.log(`${name}: ${state.stored ? 'in the store' : JSON.stringify(state.value)}`);
}
chat.close();
