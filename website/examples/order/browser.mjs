import { compileCharts, Host } from '@tinyactors/wasm';

let charges = 0;
const payments = {
  receive(send) {                   // the chart's <send type="payments">
    console.log('charge', JSON.stringify(send.data));
    if (++charges === 1) send.fail(); // down for the first charge: the order gets error.communication
    else send.answer('charged');    // an event back to the order
  },
};
const chart = await fetch(new URL('order.scxml', import.meta.url)).then((response) => response.text());
const compiled = compileCharts([{ name: 'order.scxml', text: chart }]);
const host = new Host(compiled, {
  system: 'shop',
  ioProcessors: { payments },
  log: (entry) => console.log('log', entry.text),
});
compiled.close();

const order = host.spawn('order');
order.send('add', { item: 'notebook', price: 30 });
order.send('add', { item: 'pen', price: 12 });
order.send('checkout');
console.log('paid', await order.done); // the order retried on its own
host.close();
