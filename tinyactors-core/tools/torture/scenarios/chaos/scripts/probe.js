// Probe responders every 20 ms; the reply time is the scheduling latency an
// ordinary actor sees while loopers, churners and floods compete for the thread.
const latency = bench.histogram('probeLatencyMs');
bench.on('probe', (d) => latency.record(bench.now() - d.t));
bench.on('sunk', (d) => bench.metric('sinkProcessed', d.n));
bench.every(20, () => {
  for (const id of bench.group('responders')) bench.send(id, 'probe', { t: bench.now() });
});
// keep the deep and wide charts busy
bench.every(50, () => {
  bench.broadcast(bench.group('deep'), 'swap');
  bench.broadcast(bench.group('wide'), 'flip');
});
