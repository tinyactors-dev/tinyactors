// The mail processor (host I/O processor "mail"). Idempotent by `key`: a key
// it still remembers is accepted again as a duplicate without a second
// delivery. Keys are forgotten after `keyTTLMs`. It also throttles, bounces
// and loses replies. `deliveries` counts what really reached each recipient.
const P = bench.params;
globalThis.mail = {
  outage: false,
  keyTTLMs: P.keyTTLMs ?? 30000,
  keys: new Map(), // key -> accepted at (ms)
  deliveries: new Map(), // recipient -> real deliveries
};
const r = bench.rng(23);
const latency = bench.histogram('mailLatencyMs');
bench.ioprocessor('mail', (msg) => {
  if (mail.outage) return bench.failSend(msg); // unreachable: error.communication
  const m = msg.data;
  const ms = r.int(50, 400);
  bench.after(ms, () => {
    latency.record(ms);
    const seen = mail.keys.get(m.key);
    if (seen !== undefined && bench.now() - seen < mail.keyTTLMs) {
      bench.count('mail.duplicatesDropped');
      return bench.send(msg.from, 'mail.accepted', { key: m.key, duplicate: true });
    }
    const roll = r();
    if (roll < 0.005) {
      bench.count('mail.bounced');
      return bench.send(msg.from, 'mail.rejected', { retryable: false, reason: 'no such mailbox' });
    }
    if (roll < 0.035) {
      bench.count('mail.throttled');
      return bench.send(msg.from, 'mail.rejected', { retryable: true, reason: 'throttled' });
    }
    mail.keys.set(m.key, bench.now());
    const n = (mail.deliveries.get(m.to) || 0) + 1;
    mail.deliveries.set(m.to, n);
    bench.count('delivered');
    if (n > 1) bench.count('deliveredTwice');
    if (roll < 0.045) return bench.count('mail.repliesLost'); // delivered, but nobody hears about it
    bench.send(msg.from, 'mail.accepted', { key: m.key, duplicate: false });
  });
});
// forget expired keys
bench.every(1000, () => {
  const now = bench.now();
  for (const [k, at] of mail.keys) if (now - at >= mail.keyTTLMs) mail.keys.delete(k);
  bench.metric('mail.keysRemembered', mail.keys.size);
});
bench.count('deliveredTwice', 0);
