// readSnapshot: a pack read without a host (tinyactors-core/docs/embedding.md,
// section 12). Core's readers answer the moment of the snapshot: an
// actor's state, mailbox and delayed sends, its chart's structure, and
// the spans its journal kept, while the host goes on.
import { decodeTraces, InvalidError, readSnapshot, SimulatedClock } from '@tinyactors/node';
import { equal, hostWith, scxml, throws, typeError } from './charts.mjs';
import { run, test } from './harness.mjs';

const ORDER = scxml('order', `
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="s">
    <onentry><send event="nag" id="nag" delay="10s"/></onentry>
    <transition event="order.pay" cond="n &lt; 3" target="paid"/>
    <transition event="nudge"><assign location="n" expr="n + 1"/></transition>
    <transition event="*"><assign location="n" expr="n + 1"/></transition>
  </state>
  <state id="paid"/>`);

const OPTIONS = { config: { system: { ticksPerSecond: 1000 }, telemetry: { flushInterval: 100 } }, telemetry() {} };

async function shop() {
  const clock = new SimulatedClock();
  const host = hostWith({ order: ORDER }, { ...OPTIONS, clock });
  const actor = host.spawn('order', { name: 'order-1' });
  await host.idle();
  actor.send('poke');
  actor.send('nudge');
  await host.idle();
  clock.increment(200);
  await host.idle();
  actor.send('order.pay'); // queued: the snapshot comes before a turn
  return { host, actor, clock };
}

test('a snapshot read without a host: its actors, their states, mailboxes and delayed sends', async () => {
  const { host, actor } = await shop();
  const snapshot = readSnapshot(host.snapshot());
  equal(snapshot.systemID, 't', 'its system');
  equal(snapshot.segments, 1, 'one segment');
  equal(snapshot.time, 200n, 'the host\'s time, in ticks');
  const handles = snapshot.actors(0);
  equal(handles.length, 1, 'one actor');
  const read = snapshot.actor(0, handles[0]);
  equal(read.session, actor.id, 'its session');
  equal(read.name, 'order-1', 'its name');
  equal(read.chartName, 'order', 'its chart');
  equal(read.status, 'runnable', 'an event waits for it');
  equal(read.parent, null, 'a root actor');
  equal(read.mailbox, 1, 'one event waits');
  const chart = snapshot.charts(0)[read.chart];
  equal(snapshot.configuration(0, handles[0]).map((i) => chart.states[i].id).join(), 's', 'in s');
  equal(JSON.stringify(snapshot.mailbox(0, handles[0])), '[{"event":"order.pay","sendid":null}]', 'its mailbox');
  const [timer] = snapshot.timers(0, handles[0]);
  equal(`${timer.event} ${timer.sendid} ${timer.due}`, 'nag nag 10000', 'its delayed send, due in ticks');
  const stats = snapshot.stats(0);
  equal(stats.ticksPerSecond, 1000, 'the tick rate');
  equal(stats.now, 200n, 'the segment\'s time');
  // the host went on: the event it read as waiting is taken now
  await host.idle();
  equal((await actor.state()).value, 'paid', 'the live host goes on');
  equal(snapshot.mailbox(0, handles[0]).length, 1, 'the snapshot holds its moment');
  host.close();
});

test('a chart as the snapshot\'s image has it: states, transitions with their conditions, data', async () => {
  const { host } = await shop();
  const [chart] = readSnapshot(host.snapshot()).charts(0);
  equal(chart.name, 'order', 'its name');
  equal(chart.data.join(), 'n', 'its data');
  equal(chart.states.map((s) => `${s.id}:${s.kind}:${s.parent}`).join(' '), ':compound:null s:atomic:0 paid:atomic:0', 'its states');
  const pay = chart.transitions.find((t) => t.events[0] === 'order.pay');
  equal(`${pay.cond} ${chart.states[pay.targets[0]].id} ${pay.type}`, 'n < 3 paid external', 'a transition');
  equal(chart.transitions.some((t) => t.events[0] === '*' && t.targets.length === 0), true, 'a targetless wildcard');
  equal(chart.transitions.some((t) => t.type === 'initial'), true, 'the initial transition');
  host.close();
});

test('the spans the journal kept, read from the snapshot', async () => {
  const { host } = await shop();
  const snapshot = readSnapshot(host.snapshot());
  const kept = snapshot.traceKept(0);
  equal(kept.records > 0 && kept.acknowledgedRecords <= kept.records, true, 'records kept');
  const spans = decodeTraces(snapshot.traces(0));
  equal(spans.some((s) => s.name === 'scxml.spawned'), true, 'the spawn');
  equal(spans.filter((s) => s.name.startsWith('scxml.macrostep')).length >= 2, true, 'its macrosteps');
  equal(spans.every((s) => s.start <= s.end), true, 'each span ends after it starts');
  // on the wall clock the host gave (a SimulatedClock's time), placed by the segment's own clocks
  const wall = snapshot.stats(0).wallNanoseconds;
  equal(wall > 0n, true, 'the segment was given a wall clock');
  equal(spans.every((s) => BigInt(s.start) <= wall + 1_000_000_000n && BigInt(s.end) <= wall + 1_000_000_000n), true, 'no span after the snapshot');
  // what the host exported is named as the Inspector names it: its event from the image, a
  // microstep by its macrostep, its actor by its record while resident, nothing left empty
  equal(spans.filter((s) => s.name === 'scxml.macrostep nudge' || s.name === 'scxml.microstep nudge').length, 2, 'an event of the image');
  equal(spans.some((s) => s.name === 'scxml.macrostep (unread)'), true, 'an event given at run time');
  equal(spans.some((s) => s.name.endsWith(' ')), false, 'no name left empty');
  const named = spans.filter((s) => 'tinyactors.actor.name' in s.attributes);
  equal(named.length > 0 && named.every((s) => s.attributes['tinyactors.actor.name'] === 'order-1'), true, 'actors named');
  const untraced = hostWith({ order: ORDER }, { config: { system: { tracing: false } } });
  untraced.spawn('order');
  await untraced.idle();
  const quiet = readSnapshot(untraced.snapshot());
  equal(quiet.traceKept(0), null, 'no journal');
  equal(quiet.traces(0), null, 'no spans');
  untraced.close();
  host.close();
});

test('request: core\'s snapshot methods over the whole snapshot, its errors a TinyactorsError', async () => {
  const { host, actor } = await shop();
  const snapshot = readSnapshot(host.snapshot());
  const summary = snapshot.request('snapshot.summary');
  equal(JSON.stringify(summary.states), '{"order":{"s":1}}', 'actors per resting state');
  const listed = snapshot.request('snapshot.actors', { state: 's', name: 'order-' });
  equal(listed.total, 1, 'one actor in s');
  const [row] = listed.actors;
  equal(`${row.session} ${row.states} ${row.queued} ${row.delayed[0].sendid} ${row.delayed[0].dueInMs}`, `${actor.id} s order.pay nag 9800`, 'its row');
  const chart = snapshot.request('snapshot.chart', { chart: 'order' });
  equal(chart.states.map((s) => `${s.id}:${s.actors}`).join(), 's:1,paid:0', 'actors in each state');
  const spans = snapshot.request('snapshot.spans', { session: Number(actor.id), name: 'scxml.macrostep', limit: 2 });
  equal(spans.total >= 2 && spans.spans.length === 2, true, 'the last two of its macrosteps');
  throws(() => snapshot.request('snapshot.chart', { chart: 'nothing' }), (error) => equal(`${error.reason} ${error.path}`, 'stale params.chart', 'stale'), 'a chart no segment holds');
  throws(() => snapshot.request('snapshot.nothing'), (error) => equal(error.code, -32601, 'no such method'), 'another method');
  host.close();
});

test('readSnapshot refuses what is not a snapshot; a method refuses another segment, handle or object', async () => {
  const { host } = await shop();
  throws(() => readSnapshot(new Uint8Array(64)), (error) => equal(error instanceof InvalidError, true, 'InvalidError'), 'not a snapshot');
  throws(() => readSnapshot('bytes'), typeError, 'not bytes');
  const snapshot = readSnapshot(host.snapshot());
  throws(() => snapshot.actors(1), (error) => equal(error instanceof RangeError, true, 'RangeError'), 'another segment');
  throws(() => snapshot.actor(0, 12345), (error) => equal(error instanceof RangeError, true, 'RangeError'), 'another handle');
  const Snapshot = snapshot.constructor;
  throws(() => Snapshot.prototype.actors.call({}, 0), typeError, 'another object');
  throws(() => new Snapshot(), typeError, 'made by readSnapshot only');
  host.close();
});

await run();
