# Testing your charts

A chart's logic can be tested without its I/O and without waiting for its
timeouts. Three things make that work:

- **I/O processors are your handlers**, so a test gives the host handlers
  that record what the chart sent and answer as the test decides: now,
  later, with an error, or never.
- **A `SimulatedClock`** moves only when the test moves it, so a timeout
  of an hour takes no time.
- **`await host.idle()`** returns when the host has nothing left to do,
  so the test then looks at a settled host.

## A test

The chart below asks a payment service to charge a card, retries twice
when no answer comes within 30 seconds, and gives up after the third
attempt. The test checks each step: the sends the chart made, its state
between them, and the end.

```js
import assert from 'node:assert/strict';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const chart = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="payment" initial="charging">
  <datamodel><data id="attempts" expr="0"/></datamodel>
  <state id="charging">
    <onentry>
      <assign location="attempts" expr="attempts + 1"/>
      <send type="urn:example:payments" event="charge" target="card"/>
      <send event="timeout" delay="30s" id="timer"/>
    </onentry>
    <transition event="charged" target="paid"><cancel sendid="timer"/></transition>
    <transition event="timeout" cond="attempts &lt; 3" target="charging"/>
    <transition event="timeout" target="failed"/>
  </state>
  <final id="paid"/>
  <final id="failed"><onentry><log label="payment" expr="'gave up after ' + attempts"/></onentry></final>
</scxml>`;

/** A host running the chart on a simulated clock, recording its charges. */
function setup() {
  const compiled = compileCharts([{ name: 'payment.scxml', text: chart }]);
  const clock = new SimulatedClock();
  const charges = [];
  const logs = [];
  const host = new Host({ systemID: 'test', ioProcessors: [{ type: 'urn:example:payments' }] }, {
    clock,
    ioProcessors: { 'urn:example:payments': (host, send) => charges.push({ at: clock.now, target: send.target }) },
    log: (host, entry) => logs.push(entry.text),
  });
  host.addImage(compiled.image);
  compiled.close();
  return { host, clock, charges, logs };
}

/** The actor's state value, read with a typed method. */
const state = (host, session) =>
  new Promise((resolve, reject) =>
    host.actorState({ session }, (host, result) => resolve(result.value), (host, error) => reject(error)));

// It retries every 30 seconds and gives up after three attempts.
{
  const { host, clock, charges, logs } = setup();
  const payment = host.spawn(host.chart('payment'), { name: 'payment' });
  await host.idle();
  assert.deepEqual(charges, [{ at: 0, target: 'card' }]);
  assert.equal(await state(host, payment), 'charging');

  clock.increment(30_000);
  await host.idle();
  clock.increment(30_000);
  await host.idle();
  assert.deepEqual(charges.map((charge) => charge.at), [0, 30_000, 60_000]);

  clock.increment(30_000);
  await host.idle();
  assert.deepEqual(logs, ['gave up after 3']);
  assert.equal(host.actorByName('payment'), undefined, 'a final state ends the actor');
  host.close();
  console.log('ok: retries, then gives up');
}

// An answer before the timeout ends it, and no timer is left.
{
  const { host, clock, charges } = setup();
  const payment = host.spawn(host.chart('payment'), { name: 'payment' });
  await host.idle();
  host.send(payment, 'charged');
  await host.idle();
  assert.equal(host.actorByName('payment'), undefined);
  assert.equal(host.nextDeadline, undefined, 'the timer was cancelled');
  clock.increment(60_000);
  await host.idle();
  assert.equal(charges.length, 1);
  host.close();
  console.log('ok: an answer ends it');
}
```

```output
ok: retries, then gives up
ok: an answer ends it
```

The test answered by sending `charged` itself with `host.send`, which is
what the I/O processor's `send.answer('charged')` would do, without the
`sendid`. A test may equally answer from the handler.

## Habits that help

- **Await `idle()` after each input** that should have an effect before
  the next assertion. A host runs on the event loop, so nothing has run
  right after `host.send` returns.
- **Read state with the typed methods** (`actorState`, `actorContext`)
  after an `idle()`: they read the host as it is when called
  ([Inspecting a host](inspection.md)).
- **Give actors names** to find them with `actorByName`, and to see that
  one ended: a final state ends the actor and frees its name.
- **Close every host**, in a `finally` or the test framework's teardown.
  A host on a `SimulatedClock` arms no timer, so a forgotten one does not
  keep the test process alive, but it holds its memory until collected.
- **Test against both datamodels** if the charts will run on quickjs in
  production: give the host `datamodel: 'quickjs'` in a second run
  ([The quickjs sandbox](quickjs.md)).
- **Start from a prepared world** with a [snapshot](snapshots.md): set a
  host up once, take `host.snapshot()`, and `Host.restore` it at the
  start of each test.
- **Make an unhandled error fail the test.** A handler that throws is
  reported by the next `await host.idle()`, which rejects
  ([README](../../README.md#errors)), so a test that awaits `idle()`
  sees it.
