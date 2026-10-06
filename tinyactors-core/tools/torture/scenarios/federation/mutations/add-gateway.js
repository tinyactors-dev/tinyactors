if (!bench.systems().includes('gateway-2')) {
  bench.addSystem('gateway-2', { charts: ['charts/session.scxml'], config: { maxActors: 1024 } });
  const desks = bench.group('desks');
  bench.spawn('gateway-2', 'session', { count: bench.scaled(256), group: 'sessions', data: (i) => ({ desk: bench.address(desks[(i * 7) % desks.length]) }) });
  bench.log('gateway-2 joined the federation');
}
