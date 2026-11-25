import { compileCharts, Host } from '@tinyactors/wasm';

const chart = await fetch(new URL('order.scxml', import.meta.url)).then((response) => response.text());
const compiled = compileCharts([{ name: 'order.scxml', text: chart }]);
const host = new Host(compiled, {
  system: 'shop',
  ioProcessors: {
    payments: {
      receive(send) {               // the chart's <send type="payments">
        console.log('charge', send.data.amount);
        send.answer('charged');     // an event back to the order
      },
    },
  },
  onDone: (actor, donedata) => console.log('paid', donedata.total),
});
compiled.close();

const order = host.spawn('order');
order.send('add', { price: 30 });
order.send('add', { price: 12 });
order.send('checkout');
await host.idle();
host.close();
