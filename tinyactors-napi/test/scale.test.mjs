// The scaling gate's fast pass (docs/plan.xml, p2-scale): every operation of
// tools/scale.mjs on hosts of 20,000 and 40,000 actors (the all-resident
// scenario at 40,000 and 80,000: between 20,000 and 40,000 resident
// actors a close's cost per actor steps up by 1.45 as the population leaves
// the processor's cache, then stays flat to 160,000), on the datamodel
// under test, each size in a process of its own under this runtime. The
// cost at 40,000 must be within the gate's factor of the cost at 20,000
// (per call, or per actor for the operations on the whole population); an
// operation over it is measured again before it fails, so timer noise does
// not fail the test and a scan does. The operations known to grow because
// of core (KNOWN, each with its request to core) are reported, not failed.
// `mise run scale` and `mise run test --full` run the same at two and four
// million actors.
import { Host } from '@tinyactors/node';
import { FACTOR, gate, KNOWN } from '../tools/scale.mjs';
import { CONFIG, DATAMODEL, ENGINE, image, scxml } from './charts.mjs';
import { run, test } from './harness.mjs';

test(`every operation costs at 40,000 actors at most ${FACTOR} times what it costs at 20,000`, async () => {
  const report = await gate({ actors: 20_000, 'resident-actors': 40_000, datamodel: DATAMODEL, samples: 5, 'min-ms': 20, attempts: 3 });
  for (const known of report.known) console.log(`     known: ${known}`);
  for (const [name, request] of Object.entries(KNOWN)) {
    const measured = Object.values(report.scenarios).some((scenario) => name in scenario.ratios);
    if (!measured) throw new Error(`KNOWN names ${name} (${request}), which the gate does not measure`);
  }
  if (report.failures.length) throw new Error(report.failures.join('\n'));
});

// The native datamodel's restore makes a row's values in the engine, then
// the next row's: as one value of every row, a restore of 200,000 sessions
// cost about twice per session what one of 10,000 did on node (16 us
// against 31), and a million six times (12 us against 76).
const LISTS = scxml('lists', `
  <datamodel><data id="n" expr="0"/><data id="list" expr="[1, 2, 3, 4, 5, 6, 7, 8, 'a', 'b', 'c', 'd', {x: 1}, {y: 2}]"/></datamodel>
  <state id="s"/>`);

async function restoreCost(sessions) {
  const host = new Host(CONFIG, ENGINE);
  host.addImage(image({ lists: LISTS }));
  const chart = host.chart('lists');
  for (let i = 0; i < sessions; i++) {
    host.spawn(chart);
    if (i % 4096 === 4095) await host.idle();
  }
  await host.idle();
  const snapshot = host.snapshot();
  host.close();
  let best = Infinity;
  for (let k = 0; k < 2; k++) {
    const started = performance.now();
    const restored = Host.restore(snapshot, CONFIG, ENGINE);
    best = Math.min(best, (performance.now() - started) / sessions);
    restored.close();
  }
  return best;
}

test(`a restore costs per session at 200,000 sessions at most ${FACTOR} times what it costs at 10,000`, async () => {
  if (DATAMODEL !== 'native') return; // quickjs's state is restored by core, in one call
  const small = await restoreCost(10_000);
  const large = await restoreCost(200_000);
  if (large / small > FACTOR)
    throw new Error(`restore: ${(small * 1000).toFixed(1)} us a session at 10,000, ${(large * 1000).toFixed(1)} at 200,000`);
});

await run();
