// Schema-based order stream into random gateway sessions.
const schema = bench.schema.load('data/submit.schema.json');
const r = bench.rng(5);
bench.rate(bench.scaled(4000), (n) => {
  const sessions = bench.group('sessions');
  for (let i = 0; i < n; i++) {
    const order = bench.schema.generate(schema, r);
    order.t = bench.now();
    bench.send(sessions[r.int(0, sessions.length - 1)], 'submit', order);
  }
});
