// The hot mailbox: a steady flood into a single actor.
bench.rate(bench.scaled(20000), (n) => {
  const sink = bench.group('sinks')[0];
  for (let i = 0; i < n; i++) bench.send(sink, 'drop');
  bench.count('sunk', n);
});
