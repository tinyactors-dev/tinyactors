import { readFileSync } from 'node:fs';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const clock = new SimulatedClock();
let charges = 0;
const payments = {
  receive(send) {                    // the chart's <send type="payments">
    charges += 1;
    console.log(`${clock.now} ms: charge ${send.data.amount}, attempt ${charges}`);
    if (charges > 1) send.answer('charged'); // the first charge never answers
  },
};
const email = {
  receive(send) {                    // the chart's <send type="email">
    console.log(`${clock.now} ms: receipt for ${send.data.total}`);
    send.answer('sent');
  },
};

const compiled = compileCharts([{ name: 'order.scxml', text: readFileSync('order.scxml', 'utf8') }]);
const host = new Host(compiled, { system: 'shop', clock, ioProcessors: { payments, email } });
compiled.close();

host.spawn('order', { name: 'order-1042' });
const order = host.actor('order-1042');  // any part of your program finds it by name
order.send('add', { item: 'notebook', price: 30 });
order.send('add', { item: 'pen', price: 12 });
order.send('checkout');
await host.idle();
console.log('waiting in', JSON.stringify((await order.state()).value));

clock.increment(5000);               // five seconds pass, at once: the timeout fires
await host.idle();
console.log('then in', JSON.stringify((await order.state()).value));

order.send('shipped');               // the warehouse reports the parcel
console.log('complete, total', await order.done);
host.close();
