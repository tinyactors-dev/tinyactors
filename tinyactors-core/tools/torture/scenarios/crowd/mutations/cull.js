const sensors = bench.prune('sensors');
for (let i = 0; i < sensors.length; i += 3) bench.terminate(sensors[i]);
bench.prune('sensors');
