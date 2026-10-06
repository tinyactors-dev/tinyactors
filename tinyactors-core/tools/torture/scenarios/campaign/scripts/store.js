// The checkpoint store (host I/O processor "store"): keeps the latest
// checkpoint per campaign outside the actor system, so it survives crashes.
globalThis.store = { down: false, checkpoints: new Map() };
const r = bench.rng(17);
bench.ioprocessor('store', (msg) => {
  if (store.down) return bench.failSend(msg);
  const c = msg.data;
  bench.after(r.int(2, 20), () => {
    if (store.down) return; // lost while it went down
    const prev = store.checkpoints.get(c.campaign);
    if (!prev || prev.at <= c.at) store.checkpoints.set(c.campaign, c);
    bench.send(msg.from, 'store.saved', { at: c.at });
    bench.count('checkpoints');
  });
});
