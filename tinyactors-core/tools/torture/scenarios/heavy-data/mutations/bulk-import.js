const schema = bench.schema.load('data/order.schema.json');
const r = bench.rng(Date.now() % 1000);
const ledgers = bench.group('ledgers');
const n = bench.scaled(2000);
for (let i = 0; i < n; i++) bench.send(ledgers[i % ledgers.length], 'post', bench.schema.generate(schema, r));
bench.log(`bulk import of ${n} orders`);
