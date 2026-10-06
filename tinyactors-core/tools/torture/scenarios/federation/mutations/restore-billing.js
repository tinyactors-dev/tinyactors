// Same chart, fresh system: accounts get the same session ids (1..n), so the
// desks' addresses become valid again.
bench.addSystem('billing', { charts: ['charts/account.scxml'], config: { maxActors: 256, maxEvents: 8192 } });
bench.spawn('billing', 'account', { count: federation.nAccounts, group: 'accounts' });
bench.log('billing restored');
