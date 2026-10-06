const storms = bench.prune('storms');
for (const id of storms.slice(0, Math.floor(storms.length / 2))) bench.terminate(id);
