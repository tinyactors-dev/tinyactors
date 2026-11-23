// The live chat, run in Node.js: what the page's controls do, printed as
// the screens show it. The same scenario on step 5's charts and on step
// 9's, a live migration between them (and from 9 to 5 and back, with posts
// on each side and a late joiner), a screen that loses deliveries, what a
// screen missed read from the store on step 9, the times posts keep on
// the way, a lost connection of someone who has none, and a direct message
// (made once per pair, closed to a third person, on step 9 only).
// Run: node main.mjs
import * as tinyactors from '@tinyactors/wasm';
import { readFileSync } from 'node:fs';
import { createChat, PEOPLE } from './chat.mjs';

const read = (step) => Object.fromEntries(['workspace.scxml', 'room.scxml', 'user.scxml']
  .map((name) => [name, readFileSync(new URL(`./charts/step-${step}/${name}`, import.meta.url), 'utf8')]));
const versions = { 5: read(5), 9: read(9) };
const messages = (chat, name) => chat.screen(name).messages.map((m) => m.text).join(' | ') || 'nothing';
const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
const timed = (chat, name) => chat.screen(name).messages.map((m) => `${m.text} at ${clock(m.at)}`).join(' | ') || 'nothing';

for (const version of [5, 9]) {
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

console.log('-- step 5, migrated live to step 9 and back');
const chat = await createChat(tinyactors, { versions, version: 5 });
for (const name of ['queen', 'scout']) {
  await chat.connect(name);
  await chat.join(name, 'general');
}
await chat.typing('scout', 'general');
console.log(`on step 5 queen sees typing: ${chat.screen('queen').typing.general?.join(', ') || 'nobody'}`);
const there = await chat.useVersion(9);
console.log(`to step 9: ${there.migrated} migrated, ${there.failed.length} failed`);
await chat.typing('scout', 'general');
console.log(`on step 9 queen sees typing: ${chat.screen('queen').typing.general?.join(', ') || 'nobody'}`);
await chat.loseConnection('scout', 60);
const back = await chat.useVersion(5);
console.log(`back to step 5, scout reconnecting: ${back.migrated} migrated, ${back.failed.length} failed`);
// step 5 has no reconnecting: the program signs scout out before the migration, and the reconnect
// the agenda holds signs scout in again
console.log(chat.log().find((e) => e.kind === 'version' && e.text.startsWith('scout')).text);
await chat.advance(1_000); // an event loads scout from the store
await chat.typing('scout', 'general');
const scout = await chat.state('user.scout');
console.log(`scout is ${scout.stored ? 'in the store' : JSON.stringify(scout.value)}; queen's screen shows scout as ${chat.screen('queen').roster.scout}`);
await chat.advance(60_000);
const returned = await chat.state('user.scout');
console.log(`after the reconnect scout is ${returned.stored ? 'in the store' : JSON.stringify(returned.value)}; queen's screen shows scout as ${chat.screen('queen').roster.scout}`);
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

console.log('-- step 9: what nurse\'s screen missed, read from the store');
const away = await createChat(tinyactors, { versions });
for (const name of ['forager', 'nurse']) {
  await away.connect(name);
  await away.join(name, 'general');
}
await away.disconnect('nurse');
await away.post('forager', 'general', 'while you were out');
await away.connect('nurse'); // the connection opens: the screen reads what it lacks
console.log(`signed out and back: ${messages(away, 'nurse')}`);
await away.loseConnection('nurse', 120);
await away.advance(10_000);
await away.post('forager', 'general', 'in the grace');
await away.advance(30_000); // the 30 s grace is over: nurse is offline
await away.post('forager', 'general', 'after the grace');
await away.advance(80_000); // the connection comes back
console.log(`lost for 2 minutes and back: ${messages(away, 'nurse')}`);
await away.setFaults('nurse', { drop: 1 });
await away.post('forager', 'general', 'dropped');
await away.setFaults('nurse');
await away.post('forager', 'general', 'the next one');
console.log(`a dropped delivery, then the next post: ${messages(away, 'nurse')}`);
away.close();

console.log('-- step 9: a post shows the time it was made, however late it reaches a screen');
const kept = await createChat(tinyactors, { versions });
for (const name of ['scout', 'forager']) {
  await kept.connect(name);
  await kept.join(name, 'general');
}
await kept.loseConnection('scout', 9);
await kept.advance(4_000);
await kept.post('forager', 'general', 'one');
await kept.advance(2_000);
await kept.post('forager', 'general', 'two');
await kept.advance(5_000); // scout's connection comes back at 0:09
console.log(`scout lost the connection from 0:00 to 0:09: ${timed(kept, 'scout')}`);
await kept.setFaults('scout', { delay: 10 });
await kept.post('forager', 'general', 'three');
await kept.advance(15_000);
await kept.setFaults('scout');
console.log(`a delivery 10 s late, then: ${timed(kept, 'scout')}`);
await kept.connect('nurse');
await kept.join('nurse', 'general');
console.log(`nurse joins at ${clock(kept.now())} and reads: ${timed(kept, 'nurse')}`);
kept.close();

console.log('-- a late joiner, on each version');
for (const version of [5, 9]) {
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

console.log('-- step 9 to step 5 and back, with posts on each side and a late joiner');
const trip = await createChat(tinyactors, { versions, version: 9 });
for (const name of ['scout', 'forager']) {
  await trip.connect(name);
  await trip.join(name, 'general');
}
await trip.post('forager', 'general', 'A');
await trip.post('scout', 'general', 'B');
const to5 = await trip.useVersion(5);
await trip.post('forager', 'general', 'on step 5');
const to9 = await trip.useVersion(9);
console.log(`to step 5: ${to5.migrated} migrated, ${to5.failed.length} failed; back to step 9: ${to9.migrated} migrated, ${to9.failed.length} failed`);
for (const text of ['C', 'D', 'E']) await trip.post('forager', 'general', text);
await trip.connect('queen');
await trip.join('queen', 'general');
console.log(`scout's screen got: ${messages(trip, 'scout')}`);
console.log(`forager's screen got: ${messages(trip, 'forager')}`);
console.log(`queen joined late and got: ${messages(trip, 'queen')}`);
console.log(`queen's posts carry the store's numbers: ${trip.screen('queen').messages.map((m) => m.seq).join(', ')}`);
trip.close();

console.log('-- losing the connection of someone signed out, on each version');
for (const version of [5, 9]) {
  const quiet = await createChat(tinyactors, { versions, version });
  await quiet.loseConnection('queen', 5); // nobody is signed in: queen has no connection to lose
  await quiet.advance(10_000);
  const told = quiet.log().filter((e) => e.kind === 'input' && e.text.startsWith('queen ←')).map((e) => e.text);
  const signedIn = PEOPLE.filter((name) => quiet.connected(name));
  console.log(`step ${version}: ${quiet.log().find((e) => e.kind === 'fault').text}; queen was told ${told.join(', ') || 'nothing'}; signed in: ${signedIn.join(', ') || 'nobody'}`);
  quiet.close();
}

console.log('-- step 9: scout opens a DM with forager');
const dm = await createChat(tinyactors, { versions });
for (const name of ['scout', 'forager', 'queen']) await dm.connect(name);
await dm.join('queen', 'general');
const room = await dm.openDM('scout', 'forager');
console.log(`scout opened ${room}; the channels are still: ${(await dm.rooms()).join(', ')}`);
await dm.post('scout', room, 'sugar by the oak');
const forager = dm.directMessages('forager');
console.log(`forager's DMs: ${forager.map((each) => `${each.with}, last: ${each.last.text}`).join('; ')}`);
await dm.typing('forager', room);
console.log(`scout sees typing in the DM: ${dm.screen('scout').typing[room]?.join(', ') || 'nobody'}`);
await dm.post('forager', room, 'on my way');
console.log(`opened again from forager's side: ${await dm.openDM('forager', 'scout')}`);
console.log(`scout's screen in the DM: ${dm.screen('scout').messages.filter((m) => m.room === room).map((m) => `${m.from}: ${m.text}`).join(' | ')}`);
await dm.join('queen', room);
console.log(`queen tries to join it: ${dm.screen('queen').notices.at(-1)}; queen's DMs: ${dm.directMessages('queen').length}; queen's screen holds: ${messages(dm, 'queen')}`);
console.log(`scout's own notes: ${await dm.openDM('scout', 'scout')}`);
const there5 = await dm.useVersion(5);
console.log(`to step 5: ${there5.migrated} migrated; scout opens a DM with forager: ${await dm.openDM('scout', 'forager') ?? 'none, these charts have none'}`);
await dm.useVersion(9);
await dm.post('scout', room, 'back on step 9');
const refusals = dm.screen('queen').notices.length;
await dm.join('queen', room);
console.log(`back on step 9: queen tries to join it: ${dm.screen('queen').notices.slice(refusals).join(', ') || 'let in'}; queen's screen holds: ${messages(dm, 'queen')}; forager's screen in the DM: ${dm.screen('forager').messages.filter((m) => m.room === room).map((m) => m.text).join(' | ')}`);
dm.close();
