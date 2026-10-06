// History-based events: learn a Markov chain (event sequence per sensor,
// inter-arrival gaps, payload samples) from a recorded history and replay
// its statistics against random live sensors.
const history = bench.history.load('data/sensor-history.jsonl');
const model = bench.history.markov(history, { order: 2, key: 'sensor' });
const r = bench.rng(7);
bench.rate(bench.scaled(20000), (n) => {
  const sensors = bench.group('sensors');
  if (!sensors.length) return;
  for (let i = 0; i < n; i++) {
    const k = r.int(0, sensors.length - 1);
    const e = model.next(k);
    bench.send(sensors[k], e.event, e.data);
  }
  bench.count('generated', n);
});
bench.log(`markov model from ${history.length} recorded events, mean gap ${model.meanGap.toFixed(3)} s`);
