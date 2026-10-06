const n = bench.broadcast(bench.group('sensors'), 'poke');
bench.log(`poked ${n} sensors`);
