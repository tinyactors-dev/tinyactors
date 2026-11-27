# Testing charts

A chart's logic is tested without its I/O and without waiting for its
timeouts:

- **A stand-in for every I/O processor.** It records what the chart sent
  and answers as the test decides: now, later, with a failure, or never.
  The processor boundary is the test boundary; the real processor (the
  HTTP client, the database) gets a small test of its own against a
  recorded transport.
- **A `SimulatedClock`.** It moves only when the test moves it, so a
  30-day timeout takes no time. A host made with one arms no timer.
- **`await host.idle()`** (Python: `run_until_idle()`, or `await
  host.idle()` under asyncio) returns when nothing is left to do, so the
  test then looks at a settled host.
- **Both datamodels**, on Node.js: run the same test with
  `datamodel: 'native'` and `'quickjs'`; they differ at the edges
  ([choices.md](choices.md)).
- **Warnings as errors**: compile the test's charts with
  `{ warningsAsErrors: true }`, so a syntax error in an expression fails
  the test instead of becoming an `error.execution` at run time.

## A test

A payment that retries twice when no answer comes within 30 seconds, and
gives up after the third attempt:

```js
import assert from 'node:assert/strict';
import { compileCharts, Host, SimulatedClock, StaleError } from '@tinyactors/node';

const chart = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="payment" initial="charging">
  <datamodel><data id="attempts" expr="0"/></datamodel>
  <state id="charging">
    <onentry>
      <assign location="attempts" expr="attempts + 1"/>
      <send type="payments" event="charge" target="card"/>
      <send event="timeout" delay="30s" id="timer"/>
    </onentry>
    <transition event="charged" target="paid"><cancel sendid="timer"/></transition>
    <transition event="timeout" cond="attempts &lt; 3" target="charging"/>
    <transition event="timeout" target="failed"/>
  </state>
  <final id="paid"><donedata><param name="attempts" expr="attempts"/></donedata></final>
  <final id="failed"><donedata><param name="attempts" expr="attempts"/></donedata></final>
</scxml>`;

// A host on a simulated clock, with a stand-in that records each charge.
function setup(datamodel) {
  const compiled = compileCharts([{ name: 'payment.scxml', text: chart }], { warningsAsErrors: true });
  const clock = new SimulatedClock();
  const charges = [];
  const payments = { receive: (send) => charges.push({ at: clock.now, send }) };
  const host = new Host(compiled, { system: 'test', clock, datamodel, ioProcessors: { payments } });
  compiled.close();
  return { host, clock, charges };
}

for (const datamodel of ['native', 'quickjs']) {
  // No answer: it retries every 30 seconds and gives up after three attempts.
  {
    const { host, clock, charges } = setup(datamodel);
    const payment = host.spawn('payment', { name: 'payment.1' });
    const done = payment.done;            // take it while the actor lives: once it ended, it is gone
    await host.idle();
    assert.equal((await payment.state()).value, 'charging');
    for (let i = 0; i < 3; i++) {
      clock.increment(30_000);
      await host.idle();
    }
    assert.deepEqual(charges.map((c) => c.at), [0, 30_000, 60_000]);
    assert.deepEqual(await done, { attempts: 3 });
    assert.throws(() => host.actor('payment.1'), StaleError);
    host.close();
  }
  // An answer before the timeout: paid, and no timer is left.
  {
    const { host, clock, charges } = setup(datamodel);
    const payment = host.spawn('payment');
    const done = payment.done;
    await host.idle();
    payment.send('charged');
    assert.deepEqual(await done, { attempts: 1 });
    assert.equal(host.nextDeadline, undefined);
    clock.increment(60_000);
    await host.idle();
    assert.equal(charges.length, 1);
    host.close();
  }
  console.log(`ok on ${datamodel}`);
}
```

```output
ok on native
ok on quickjs
```

## What to assert

- **What the chart sent**, through the stand-in: the event, the data, the
  target, and when (`clock.now`).
- **Its state** (`await actor.state()`, `.value`) and **variables**
  (`await actor.context()`, `.variables`) between steps.
- **How it ended**: `actor.done` (its donedata), taken while the actor
  lives (an actor that already ended gives a rejected `done`), or a
  `StaleError` from `host.actor(name)` once it ended.
- **Its timers**: `host.nextDeadline` (milliseconds to the next one, or
  `undefined`), or `(await actor.get()).timers`.
- **Failures as events**: a stand-in that calls `send.fail()` gives the
  chart `error.communication`; check the chart handles it.

## The clock

`clock.increment(ms)` moves the clock once: what is due by then runs once,
at the new time, and a delay it starts counts from the new time. To see
every tick of a repeating timer, step to each deadline:
`clock.increment(host.nextDeadline)`. An input given just before an
increment runs at the new time; `await host.idle()` between them when it
should run first.

## The real processors

Test each real I/O processor once against a recorded transport (a fake
`fetch`, a database in memory): what it sends, and what it makes of each
answer, a refusal and an outage included. The charts' tests never need
it.

## The guides

`docs/guide/testing.md` in `@tinyactors/node`, and the Python README's
"Drivers and time", have more: stepping the clock, typed methods in
tests, testing with a store.
