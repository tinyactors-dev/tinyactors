// Schema-based events: orders generated from data/order.schema.json.
const schema = bench.schema.load('data/order.schema.json');
const r = bench.rng(3);
bench.rate(bench.scaled(1500), (n) => {
  const ledgers = bench.group('ledgers');
  for (let i = 0; i < n; i++) bench.send(ledgers[r.int(0, ledgers.length - 1)], 'post', bench.schema.generate(schema, r));
});
bench.on('posted', (data) => {
  bench.count('posted');
  bench.metric('journalLength', data.n);
});
bench.on('rebuilt', (data) => bench.count('rebuilt') && bench.metric('rebuiltEntries', data.entries));
