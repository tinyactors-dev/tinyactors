// Wire the federation: accounts first (deterministic session ids survive a
// billing restart), desks know an account, sessions know a desk.
const nAccounts = bench.scaled(16), nDesks = bench.scaled(32), nSessions = bench.scaled(512);
globalThis.federation = { nAccounts };
const accounts = bench.spawn('billing', 'account', { count: nAccounts, group: 'accounts' });
const desks = bench.spawn('orders', 'desk', { count: nDesks, group: 'desks', data: (i) => ({ billing: bench.address(accounts[i % accounts.length]) }) });
bench.spawn('gateway', 'session', { count: nSessions, group: 'sessions', data: (i) => ({ desk: bench.address(desks[i % desks.length]) }) });
bench.log(`federation: ${nAccounts} accounts ← ${nDesks} desks ← ${nSessions} sessions`);
const latency = bench.histogram('orderLatencyMs');
bench.on('confirmed', (d) => {
  bench.count('confirmed');
  latency.record(bench.now() - d.t);
});
bench.on('rejected', () => bench.count('rejected'));
bench.on('lost', () => bench.count('lost'));
