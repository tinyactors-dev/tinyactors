// Ramp to `target` sensors in chunks, so growth by relocation is visible.
const target = bench.scaled(100000);
const chunk = Math.max(1, Math.ceil(target / 100)); // ~10 s at 100 ms per chunk
let spawned = 0;
const t = bench.every(100, () => {
  const n = Math.min(chunk, target - spawned);
  bench.spawn('crowd', 'sensor', { count: n, group: 'sensors' });
  spawned += n;
  bench.count('spawned', n);
  if (spawned >= target) {
    bench.cancel(t);
    bench.log(`ramp complete: ${spawned} sensors`);
  }
});
bench.every(250, () => {
  const s = bench.stats('crowd');
  if (s && s.actors) bench.metric('bytesPerActor', s.regionBytes / s.actors);
});
bench.check('ramp reached its target (≥ 90%)', (summary) => summary.systems.crowd.actors >= 0.9 * target || summary.duration < 12);
