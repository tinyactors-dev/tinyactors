// The live chat, run in Node.js: what the page's controls do, printed as
// the screens show it. The same scenario on step 5's charts and on step
// 8's, a live migration between them, and a screen that loses deliveries.
// Run: node main.mjs
import * as tinyactors from '@tinyactors/wasm';
import { readFileSync } from 'node:fs';
import { createChat } from './chat.mjs';

const read = (step) => Object.fromEntries(['workspace.scxml', 'room.scxml', 'user.scxml']
  .map((name) => [name, readFileSync(new URL(`./charts/step-${step}/${name}`, import.meta.url), 'utf8')]));
const versions = { 5: read(5), 8: read(8) };
const messages = (chat, name) => chat.screen(name).messages.map((m) => m.text).join(' | ') || 'nothing';

for (const version of [5, 8]) {
  const chat = await createChat(tinyactors, { versions, version });
  await chat.scenario('tunnel');
  await chat.advance(2_000);
  console.log(`-- step ${version}, scout in the tunnel`);
  console.log(`forager sees scout as ${chat.screen('forager').roster.scout}`);
  await chat.advance(118_000);
  console.log(`scout's screen got: ${messages(chat, 'scout')}`);
  const offline = chat.log().filter((e) => e.text === 'forager sees scout is offline').length;
  console.log(`scout went offline on forager's screen ${offline === 1 ? 'once' : `${offline} times`}`);
  chat.close();
}

console.log('-- step 5, migrated live to step 8 and back');
const chat = await createChat(tinyactors, { versions, version: 5 });
for (const name of ['queen', 'scout']) {
  await chat.connect(name);
  await chat.join(name, 'general');
}
await chat.typing('scout', 'general');
console.log(`on step 5 queen sees typing: ${chat.screen('queen').typing.general?.join(', ') || 'nobody'}`);
const there = await chat.useVersion(8);
console.log(`to step 8: ${there.migrated} migrated, ${there.failed.length} failed`);
await chat.typing('scout', 'general');
console.log(`on step 8 queen sees typing: ${chat.screen('queen').typing.general?.join(', ') || 'nobody'}`);
await chat.loseConnection('scout', 60);
const back = await chat.useVersion(5);
console.log(`back to step 5, scout reconnecting: ${back.migrated} migrated, ${back.failed.length} failed`);
// step 5 has no reconnecting: the migration puts scout in the chart's initial state, offline,
// and nothing tells the workspace, so queen's screen still shows scout active
await chat.advance(1_000); // an event loads scout from the store
await chat.typing('scout', 'general');
const scout = await chat.state('user.scout');
console.log(`scout is ${scout.stored ? 'in the store' : JSON.stringify(scout.value)}; queen's screen shows scout as ${chat.screen('queen').roster.scout}`);
chat.close();

console.log('-- forager\'s screen loses half of what reaches it');
const flaky = await createChat(tinyactors, { versions, seed: 7 });
for (const name of ['scout', 'forager']) {
  await flaky.connect(name);
  await flaky.join(name, 'general');
}
await flaky.setFaults('forager', { drop: 0.5 });
for (const text of ['one', 'two', 'three', 'four', 'five', 'six']) await flaky.post('scout', 'general', text);
console.log(`scout's screen got: ${messages(flaky, 'scout')}`);
console.log(`forager's screen got: ${messages(flaky, 'forager')}`);
console.log(`lost on the way: ${flaky.log().filter((e) => e.kind === 'lost').length}`);
flaky.close();

console.log('-- a late joiner, on each version');
for (const version of [5, 8]) {
  const late = await createChat(tinyactors, { versions, version });
  await late.connect('scout');
  await late.join('scout', 'general');
  await late.post('scout', 'general', 'sugar by the oak');
  await late.join('scout', 'general'); // a second join: on step 5 the room adds scout again
  await late.connect('nurse');
  await late.join('nurse', 'general');
  await late.post('scout', 'general', 'bring a friend');
  console.log(`step ${version}: nurse's screen got: ${messages(late, 'nurse')}; scout's: ${messages(late, 'scout')}`);
  late.close();
}
