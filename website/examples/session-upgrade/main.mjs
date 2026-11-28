import { readFileSync } from 'node:fs';
import { compileCharts, Host, MemoryStore } from '@tinyactors/node';

const image = (file) => {
  const compiled = compileCharts([{ name: file, text: readFileSync(file, 'utf8') }]);
  const bytes = compiled.image;
  compiled.close();
  return bytes;
};

let completions = 0;
const model = { receive(send) { completions++; send.answer('reply'); } }; // a stand-in for the model's API
const store = new MemoryStore(); // a real program passes its database: SQLiteStore, or its own
const limited = []; // the turns of each session that ended at the limit
let migrationDone;
const migrated = new Promise((resolve) => { migrationDone = resolve; });
const host = new Host(image('session-v1.scxml'), {
  system: 'agents',
  ioProcessors: { model },
  store,
  resident: 100, // at most 100 sessions in memory, the rest in the store
  migrationDone, // called with the migration's result when it ends
  onDone: (actor, donedata) => { limited.push(donedata.turns); },
});

const sessions = Array.from({ length: 10_000 }, () => host.spawn('session'));
for (let turn = 0; turn < 3; turn++) {
  for (const session of sessions) session.send('prompt', { text: 'Summarize my inbox' });
  await host.idle();
}
console.log(`version 1: ${completions} completions, ${store.blobs.size} sessions in the store`);

host.addImage(image('session-v2.scxml')); // the new rules, added while the host runs
host.migrate({ chart: 'session', wake: true });
const result = await migrated;
console.log(`migration: ${result.migrated} sessions moved, ${result.failed.length} failed`);

completions = 0;
for (const session of sessions) session.send('prompt', { text: 'One more' });
await host.idle();
const afterThree = limited.filter((turns) => turns === 3).length;
console.log(`version 2: ${completions} completions, ${afterThree} sessions ended at the limit of 3 turns`);
host.close();
