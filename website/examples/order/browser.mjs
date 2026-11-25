import { compileCharts, Host } from '@tinyactors/wasm';

const payments = {
  up: false,                        // the payment service is down at first
  receive(send) {                   // the chart's <send type="payments">
    console.log('charge', JSON.stringify(send.data));
    if (this.up) send.answer('charged'); // an event back to the order
    else send.fail();               // the order gets error.communication
  },
};
const chart = await fetch(new URL('order.scxml', import.meta.url)).then((response) => response.text());
const compiled = compileCharts([{ name: 'order.scxml', text: chart }]);
const host = new Host(compiled, {
  system: 'shop',
  ioProcessors: { payments },
  log: (entry) => console.log('log', entry.text),
  onDone: (actor, donedata) => console.log('paid', donedata),
});
compiled.close();

const order = host.spawn('order');
order.send('add', { item: 'notebook', price: 30 });
order.send('add', { item: 'pen', price: 12 });
order.send('checkout');
await host.idle();
payments.up = true;
order.send('checkout');             // the cart kept its lines: check out again
await host.idle();
host.close();
