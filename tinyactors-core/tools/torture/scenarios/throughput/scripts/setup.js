// Pair pingers with pongers: each pinger learns its ponger's SCXML address.
const pairs = bench.scaled(64);
const pongers = bench.spawn('echo', 'ponger', { count: pairs, group: 'pongers' });
bench.spawn('echo', 'pinger', { count: pairs, group: 'pingers', data: (i) => ({ peer: bench.address(pongers[i]) }) });
bench.log(`${pairs} ping-pong pairs, ${bench.group('storms').length} storms`);
