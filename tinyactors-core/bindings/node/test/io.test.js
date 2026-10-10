'use strict';
// The outside world (SCXML §6.4, §C): I/O processors take <send type>, and
// their failures come back as error.communication; invokers run <invoke
// type>, send events to the invoking session, end it with done.invoke or
// error.communication, receive what it sends to #_<invokeid> and what it
// autoforwards, and are aborted when the state exits.
const test = require('node:test');
const assert = require('node:assert/strict');
const { fromSCXML, createSystem, createActor } = require('./binding');
const { until } = require('./charts');

const PAY = 'https://shop.dev/io/payments';
const STOCK = 'https://shop.dev/invoke/stock';

const shop = fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="shop" initial="paying">
  <datamodel><data id="log" expr="[]"/><data id="where" expr="''"/></datamodel>
  <state id="paying">
    <onentry>
      <assign location="where" expr="_ioprocessors['${PAY}'].location"/>
      <send type="${PAY}" target="card" event="charge" id="c1"><param name="amount" expr="5"/></send>
      <send type="${PAY}" target="card" event="refuse" id="c2"/>
      <send type="${PAY}" target="card" event="reject" id="c3"/>
    </onentry>
    <transition event="error.communication"><script>log.push('failed ' + _event.sendid)</script></transition>
    <transition event="go" target="stocking"/>
  </state>
  <state id="stocking">
    <invoke type="${STOCK}" src="stock:x" id="inv" autoforward="true"><param name="sku" expr="'x'"/></invoke>
    <transition event="reserved"><script>log.push('reserved ' + _event.data.sku + ' ' + _event.invokeid)</script><send target="#_inv" event="thanks"/></transition>
    <transition event="done.invoke.inv" target="stocked"><script>log.push('done ' + _event.data.n)</script></transition>
    <transition event="error.communication" target="failed"/>
    <transition event="leave" target="left"/>
  </state>
  <state id="stocked"/><state id="failed"/><state id="left"/>
</scxml>`);

for (const ecmascript of ['native', 'quickjs']) {
  test(`${ecmascript}: I/O processors: <send type> reaches send(); a throw or a rejection is error.communication`, async () => {
    const sent = [];
    const system = createSystem({
      systemID: 'io',
      ecmascript,
      ioProcessors: {
        [PAY]: {
          location: 'payments',
          send(message, context) {
            sent.push({ ...message, signal: context.signal instanceof AbortSignal, system: context.system === system });
            if (message.event === 'refuse') throw new Error('no');
            if (message.event === 'reject') return Promise.reject(new Error('later no'));
            return undefined;
          },
        },
      },
      invokers: { [STOCK]: { start() {} } },
    });
    const a = createActor(shop, { system }).start();
    await until(() => a.getSnapshot().context.log.length === 2);
    assert.deepEqual(a.getSnapshot().context.log, ['failed c2', 'failed c3']);
    assert.equal(a.getSnapshot().context.where, 'payments', '_ioprocessors shows the location');
    assert.deepEqual(sent[0], { target: 'card', event: 'charge', sendid: 'c1', data: { amount: 5 }, origin: a.address, signal: true, system: true });
    assert.equal(sent.length, 3);
    system.stop();
  });

  test(`${ecmascript}: invokers: start, send to the session, receive #_<invokeid> and autoforwarded events, done`, async () => {
    const seen = [];
    let invocation;
    const system = createSystem({
      ecmascript,
      ioProcessors: { [PAY]: { location: '', send() {} } },
      invokers: {
        [STOCK]: {
          start(inv, context) {
            invocation = { inv, context };
            seen.push(['start', inv]);
            setTimeout(() => context.send('reserved', { sku: inv.params.sku }), 1);
          },
          receive(invokeid, event) {
            seen.push(['receive', invokeid, event.name]);
          },
        },
      },
    });
    const a = createActor(shop, { system }).start();
    a.send({ type: 'go' });
    await until(() => a.getSnapshot().context.log.length === 1);
    assert.deepEqual(seen[0], ['start', { invokeid: 'inv', type: STOCK, src: 'stock:x', params: { sku: 'x' } }]);
    assert.equal(a.getSnapshot().context.log[0], 'reserved x inv');
    a.send({ type: 'poke' }); /* autoforwarded */
    /* autoforward copies every external event, the service's own reserved too (the core decides) */
    assert.deepEqual(seen.slice(1).map((x) => x[2]).filter((name) => name !== 'reserved'), ['thanks', 'poke']);
    assert.ok(seen.slice(1).every(([kind, invokeid]) => kind === 'receive' && invokeid === 'inv'));
    invocation.context.done({ n: 3 });
    await until(() => a.getSnapshot().value === 'stocked');
    assert.equal(a.getSnapshot().context.log[1], 'done 3');
    system.stop();
  });

  test(`${ecmascript}: invokers: fail(), a throwing start, no invoker: error.communication; exits abort`, async () => {
    let mode = 'fail';
    let aborted = 0;
    const system = createSystem({
      ecmascript,
      ioProcessors: { [PAY]: { location: '', send() {} } },
      invokers: {
        [STOCK]: {
          start(inv, context) {
            context.signal.addEventListener('abort', () => aborted++);
            if (mode === 'throw') throw new Error('cannot');
            if (mode === 'fail') setTimeout(() => context.fail(), 1);
          },
        },
      },
    });
    const a = createActor(shop, { system }).start();
    a.send({ type: 'go' });
    await until(() => a.getSnapshot().value === 'failed');
    mode = 'throw';
    const b = createActor(shop, { system }).start();
    b.send({ type: 'go' });
    await until(() => b.getSnapshot().value === 'failed');
    mode = 'stay';
    const c = createActor(shop, { system }).start();
    c.send({ type: 'go' });
    c.send({ type: 'leave' });
    assert.equal(c.getSnapshot().value, 'left');
    assert.equal(aborted, 3, 'each invocation is aborted once');
    system.stop();
    const none = createSystem({ ecmascript, ioProcessors: { [PAY]: { location: '', send() {} } } });
    const d = createActor(shop, { system: none }).start();
    d.send({ type: 'go' });
    await until(() => d.getSnapshot().value === 'failed');
    none.stop();
  });
}

test('stopping the system aborts its invocations and I/O signals', async () => {
  let signal;
  let ioSignal;
  const system = createSystem({
    ioProcessors: { [PAY]: { location: '', send: (m, context) => void (ioSignal = context.signal) } },
    invokers: { [STOCK]: { start: (inv, context) => void (signal = context.signal) } },
  });
  createActor(shop, { system }).start().send({ type: 'go' });
  assert.equal(signal.aborted, false);
  system.stop();
  assert.equal(signal.aborted, true);
  assert.equal(ioSignal.aborted, true);
});
