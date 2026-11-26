// @tinyactors/mcp over a host of this build: the tools an agent calls,
// through an MCP client on an in-memory transport. Decoding a snapshot of
// this build also checks lib/snapshot.mjs, the copy of the website
// Inspector's decoder, against the layout core writes now.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';
import { createMCP, tap } from '../index.mjs';

const datamodel = process.env.TA_NAPI_DATAMODEL ?? 'native';

const order = (version) => `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="order" initial="paying">
  <datamodel><data id="amount" expr="_event.data ? _event.data.amount : 0"/><data id="tries" expr="0"/></datamodel>
  <state id="paying">
    <onentry><log label="order" expr="'paying v${version}'"/></onentry>
    <invoke id="pay" type="payment" src="payments:charge"><param name="amount" expr="amount"/></invoke>
    <transition event="payment.ok" target="${version === 1 ? 'paid' : 'shipping'}"/>
    <transition event="ping"><assign location="tries" expr="tries + 1"/></transition>
  </state>
  ${version === 1 ? '<state id="paid"/>' : '<state id="shipping"/>'}
</scxml>`;
const image = (version) => {
  const compiled = compileCharts([{ name: 'order.scxml', text: order(version) }]);
  const bytes = compiled.image;
  compiled.close();
  return bytes;
};

async function setup(options = {}) {
  const clock = new SimulatedClock();
  const t = tap();
  const host = new Host(image(1), t.options({
    system: 'shop',
    config: { system: { ticksPerSecond: 1000 }, telemetry: { flushInterval: 100 } },
    datamodel,
    clock,
    services: { payment: (invocation) => ({ amount: invocation.data.amount }) },
  }));
  const mcp = createMCP({ shop: { host, tap: t, sources: { order: order(1) } } }, options);
  const [client, server] = InMemoryTransport.createLinkedPair();
  await mcp.server().connect(server);
  const agent = new Client({ name: 'test', version: '0' });
  await agent.connect(client);
  const call = async (name, args = {}) => {
    const result = await agent.callTool({ name, arguments: args });
    return { ...result, value: result.structuredContent };
  };
  return { host, clock, t, agent, call, close: async () => { await agent.close(); host.close(); } };
}

test('the tools an agent sees', async () => {
  const { agent, close } = await setup();
  const names = (await agent.listTools()).tools.map((tool) => tool.name).sort();
  assert.deepEqual(names, ['actor', 'ask', 'chart_source', 'compile', 'deploy', 'end_service', 'host_stats', 'logs', 'rehearse', 'send',
    'services', 'snapshot', 'snapshot_actors', 'snapshot_chart', 'snapshot_summary', 'snapshot_traces', 'snapshot_variables', 'systems',
    'terminate', 'trace_unwatch', 'trace_watch', 'traces'].sort());
  await close();
});

test('live: systems, an actor, a send, its logs, traces and services', async () => {
  const { host, clock, call, close } = await setup();
  const a = host.spawn('order', { name: 'order-1', params: { amount: 40 } });
  host.spawn('order', { name: 'order-2', params: { amount: 70 } });
  await host.idle();

  const [shop] = (await call('systems')).value.items;
  assert.equal(shop.system, 'shop');
  assert.equal(shop.actors, 2);

  const sent = await call('send', { system: 'shop', actor: 'order-1', event: 'ping' });
  assert.equal(sent.isError, undefined);
  await host.idle();
  const actor = (await call('actor', { system: 'shop', actor: String(a.id) })).value;
  assert.equal(actor.context.variables.tries, 1);

  const logs = (await call('logs', { system: 'shop' })).value;
  assert.equal(logs.total, 2);
  assert.equal(logs.logs[0].text, 'paying v1');

  const services = (await call('services', { system: 'shop' })).value.items;
  assert.deepEqual(services.map((s) => [s.invokeid, s.type]), [['pay', 'payment'], ['pay', 'payment']]);

  clock.increment(200);
  await host.idle();
  const traces = (await call('traces', { system: 'shop' })).value;
  assert.ok(traces.total > 0, 'the tap kept the spans the host exported');

  const off = await call('terminate', { system: 'shop', actor: 'order-1' });
  assert.equal(off.isError, true, 'terminate is off unless the server allows it');
  const unknown = await call('actor', { system: 'shop', actor: 'order-9' });
  assert.equal(unknown.isError, true);
  await close();
});

test('a snapshot: summary, actors by state, variables from a copy, the chart, its traces', async () => {
  const { host, clock, call, close } = await setup();
  for (let i = 1; i <= 3; i++) host.spawn('order', { name: `order-${i}`, params: { amount: 10 * i } });
  await host.idle();
  host.actor('order-2').send('ping');
  clock.increment(200);
  await host.idle();

  const snap = (await call('snapshot', { system: 'shop' })).value;
  assert.equal(snap.actors, 3);

  const summary = (await call('snapshot_summary', { snapshot: snap.snapshot })).value;
  assert.deepEqual(summary.states, { order: { paying: 3 } });

  const paying = (await call('snapshot_actors', { snapshot: snap.snapshot, state: 'paying' })).value;
  assert.deepEqual(paying.actors.map((r) => r.name), ['order-1', 'order-2', 'order-3']);
  assert.equal(paying.actors[0].status, 'idle');

  const variables = (await call('snapshot_variables', { snapshot: snap.snapshot, chart: 'order' })).value.actors;
  assert.deepEqual(variables.map((v) => [v.variables.amount, v.variables.tries]), [[10, 0], [20, 1], [30, 0]]);

  const chart = (await call('snapshot_chart', { snapshot: snap.snapshot, chart: 'order' })).value;
  assert.deepEqual(chart.states.map((s) => [s.id, s.actors]), [['paying', 3], ['paid', 0]]);
  assert.deepEqual(chart.data, ['amount', 'tries']);

  const traces = await call('snapshot_traces', { snapshot: snap.snapshot });
  assert.equal(traces.isError, undefined);
  assert.ok(traces.value.total > 0);

  // the live host went on untouched: the copy's actors and services were stand-ins
  assert.equal((await host.hostStats()).host.actors, 3);
  await close();
});

test('compile, rehearse on a copy, then deploy and migrate: the service goes on', async () => {
  const { host, call, t, close } = await setup({ allowDeploy: true });
  for (let i = 1; i <= 2; i++) host.spawn('order', { name: `order-${i}`, params: { amount: 10 * i } });
  await host.idle();

  const broken = await call('compile', { name: 'order.scxml', text: '<scxml' });
  assert.equal(broken.isError, true);
  const draft = (await call('compile', { name: 'order.scxml', text: order(2) })).value.draft;

  const snap = (await call('snapshot', { system: 'shop' })).value.snapshot;
  const rehearsal = (await call('rehearse', { snapshot: snap, draft, chart: 'order', services: { payment: 'payment.ok' } })).value;
  assert.equal(rehearsal.result.migrated, 2);
  assert.deepEqual(rehearsal.before, { paying: 2 });
  assert.deepEqual(rehearsal.after, { shipping: 2 }, 'the stand-in answered, and v2 took the answer');
  assert.deepEqual(rehearsal.invocations, { 'payment (restored)': 2 });

  const deployed = (await call('deploy', { system: 'shop', draft, chart: 'order', migrate: true })).value;
  assert.equal(deployed.result.migrated, 2);
  assert.deepEqual(deployed.result.failed, {});
  // the live services kept running through the migration: answer one
  const [invocation] = t.state.services.get(host.actor('order-1').id);
  invocation.sendBack('payment.ok');
  await host.idle();
  assert.equal((await host.actor('order-1').state()).value, 'shipping');
  assert.equal((await call('chart_source', { system: 'shop', chart: 'order' })).content[0].text, order(2));

  const ended = await call('end_service', { system: 'shop', session: Number(host.actor('order-2').id), invokeid: 'pay', as: 'error' });
  assert.equal(ended.isError, undefined);
  await close();
});

test('listen: Streamable HTTP with a bearer token', async () => {
  const { host } = await setup();
  const t = tap();
  const mcp = createMCP({ shop: { host, tap: t } });
  const served = await mcp.listen({ token: 'secret' });
  const refused = await fetch(served.url, { method: 'POST' });
  assert.equal(refused.status, 401);
  const { StreamableHTTPClientTransport } = await import('@modelcontextprotocol/sdk/client/streamableHttp.js');
  const agent = new Client({ name: 'test', version: '0' });
  await agent.connect(new StreamableHTTPClientTransport(new URL(served.url), { requestInit: { headers: { authorization: 'Bearer secret' } } }));
  const result = await agent.callTool({ name: 'systems', arguments: {} });
  assert.equal(result.structuredContent.items[0].system, 'shop');
  await agent.close();
  await served.close();
  host.close();
});

test('lib/snapshot.mjs is the website Inspector\'s decoder, but for its header', async () => {
  const { readFileSync, existsSync } = await import('node:fs');
  const website = new URL('../../../website/examples/inspector/decode.mjs', import.meta.url);
  if (!existsSync(website)) return; // an installed package: the monorepo is not there
  const copy = readFileSync(new URL('../lib/snapshot.mjs', import.meta.url), 'utf8');
  assert.equal(copy.slice(copy.indexOf('\n\n') + 2), readFileSync(website, 'utf8'));
});
