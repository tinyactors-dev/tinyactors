// Reload: CGI-style hot reloading and live migration under traffic.
//
// traffic   numbered jobs at `rate`/s through a router (system "front") to
//           workers by address: 80% to short-lived workers (spawned on demand
//           in the newest version, `lifetime` jobs each), 20% to residents
// reloads   at 20%, 40%, 60% of the run a new version of worker.scxml
//           (charts/worker-next.scxml with the number filled in)
// migrate   10% after each reload, residents move to the newest version
// drain     the last 15%: no new jobs; short-lived workers get their remaining
//           jobs and finish, so old versions empty out and are freed

const P = bench.params;
const D = bench.duration() * 1000;
const cfg = {
  residents: Number(P.residents ?? bench.scaled(400)),
  rate: Number(P.rate ?? bench.scaled(4000)), // jobs per second
  lifetime: Number(P.lifetime ?? 20), // jobs per short-lived worker
  reloads: Number(P.reloads ?? 3),
};
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

bench.addSystem('front', { charts: ['charts/router.scxml'] });
bench.addSystem('workers', {
  charts: ['charts/worker.scxml'],
  config: { maxActors: 4096, maxEvents: 16384, maxTimers: 8192, quantum: 400 },
});
const router = bench.spawn('front', 'router')[0];
const residents = bench.spawn('workers', 'worker', { count: cfg.residents, data: { limit: 0 } }).map((id) => bench.address(id));
const residentSet = new Set(residents);

const st = {
  version: 1,
  seq: 0,
  sending: true,
  pool: [], // short-lived workers with jobs left: { addr, left }
  unacked: new Set(),
  sentTo: new Map(), // seq -> address (diagnostics for unacknowledged jobs)
  dupInfo: [],
  acksByVersion: {},
  lastN: new Map(), // worker address -> count in its last ack
  gaps: 0,
  dups: 0,
  upgrades: 0,
  upgradeMismatch: 0,
  renamed: 0,
  migrated: 0,
  leftBehind: 0,
  hibernated: 0,
  migrations: [],
  timeoutsAfterMove: 0,
  undeliverable: 0,
  transients: 0,
};
globalThis.reload = st;
const r = bench.rng(11);
const template = bench.readText('charts/worker-next.scxml');
const poolSize = Math.max(2, Math.ceil(cfg.rate / 200));

function sendJob(addr) {
  const seq = ++st.seq;
  st.unacked.add(seq);
  st.sentTo.set(seq, addr);
  bench.send(router, 'job', { to: addr, seq });
  bench.count('jobsSent');
}

function shortLived() {
  while (st.pool.length < poolSize) {
    const id = bench.spawn('workers', 'worker', { data: { limit: cfg.lifetime } })[0];
    st.pool.push({ addr: bench.address(id), left: cfg.lifetime });
    st.transients++;
  }
  const i = r.int(0, st.pool.length - 1);
  const w = st.pool[i];
  if (--w.left === 0) st.pool.splice(i, 1); // it finishes after this job
  return w.addr;
}

bench.rate(cfg.rate, (n) => {
  if (!st.sending) return;
  for (let k = 0; k < n; k++) sendJob(r() < 0.8 ? shortLived() : residents[r.int(0, residents.length - 1)]);
});

st.next = () => {
  st.version++;
  bench.reload('workers', [{ name: 'worker.scxml', text: template.replaceAll('{{VERSION}}', String(st.version)) }]);
};
st.migrate = (opts = {}) => {
  const t0 = bench.now();
  bench.migrate('workers', {
    ...opts,
    timeoutMs: 5000, // whoever is still busy then stays on its version
    onDone: (m) => {
      st.migrated += m.migrated;
      st.leftBehind += m.pending;
      st.migrations.push({ t: bench.now() / 1000, version: st.version, ms: bench.now() - t0, ...m });
      bench.log(`migrated ${fmt(m.migrated)} workers to version ${st.version} in ${Math.round(bench.now() - t0)} ms (${m.failed} failed, ${m.pending} left behind)`);
    },
  });
};

for (let k = 0; k < cfg.reloads; k++) {
  bench.after(D * (0.2 + 0.2 * k), st.next);
  bench.after(D * (0.3 + 0.2 * k), () => st.migrate({ wake: k === 0 }));
}
// before the first migration some residents sleep (unloaded to the bench's blob file):
// two revolutions of the CLOCK sweep, in chunks of 2048 slots per timer tick, catch
// those that did not run in between; the first migration wakes them (`wake: true`)
// instead of leaving them on version 1 until their next timeout
bench.after(D * 0.27, () => {
  const chunk = 2048;
  let left = 2 * Math.ceil(bench.stats('workers').actorsCap / chunk);
  const step = () => {
    st.hibernated += bench.hibernate('workers', { scan: chunk });
    if (--left > 0) bench.after(1, step);
    else bench.log(`${fmt(st.hibernated)} residents hibernated before the migration`);
  };
  step();
});
bench.after(D * 0.85, () => {
  st.sending = false; // drain: short-lived workers get their remaining jobs
  for (const w of st.pool.splice(0)) while (w.left-- > 0) sendJob(w.addr);
  bench.log(`traffic stopped after ${fmt(st.seq)} jobs; draining`);
});

if (P.watch) // CGI style: save charts/worker-next.scxml and it is the next version
  bench.watch('workers', ['charts/worker-next.scxml'], {
    load: () => [{ name: 'worker.scxml', text: bench.readText('charts/worker-next.scxml').replaceAll('{{VERSION}}', String(++st.version)) }],
    then: () => st.migrate(),
  });

bench.on('ack', (d) => {
  if (!st.unacked.delete(d.seq)) {
    st.dups++;
    if (st.dupInfo.length < 10) st.dupInfo.push({ seq: d.seq, who: d.who, v: d.v, n: d.n, t: bench.now() });
  }
  st.sentTo.delete(d.seq);
  bench.count('jobsAcked');
  st.acksByVersion[d.v] = (st.acksByVersion[d.v] || 0) + 1;
  const prev = st.lastN.get(d.who);
  if (prev !== undefined && d.n !== prev + 1) st.gaps++;
  st.lastN.set(d.who, d.n);
});
bench.on('upgraded', (d) => {
  st.upgrades++;
  if (d.renamed) st.renamed++;
  if ((st.lastN.get(d.who) ?? 0) !== d.n) st.upgradeMismatch++;
});
bench.on('timeout', (d) => {
  if (d.v > 1 && residentSet.has(d.who)) st.timeoutsAfterMove++;
});
bench.on('undeliverable', () => st.undeliverable++);

bench.describeMetric('versions', { title: 'Worker versions alive', unit: 'num' });
bench.every(250, () => {
  const vs = bench.versions('workers');
  bench.metric('versions.alive', vs.filter((v) => v.actors > 0 || v.current).length);
  bench.metric('versions.current', st.version);
  bench.metric('jobs.unacked', st.unacked.size);
  bench.metric('migrated', st.migrated);
});

bench.onFinish((s) => {
  const vs = bench.versions('workers');
  s.reload = {
    versions: vs,
    jobs: st.seq,
    lost: st.unacked.size,
    unacked: [...st.unacked].slice(0, 10).map((q) => ({ seq: q, to: st.sentTo.get(q) })),
    dupInfo: st.dupInfo,
    migrations: st.migrations,
    dups: st.dups,
    gaps: st.gaps,
    acksByVersion: st.acksByVersion,
    migrated: st.migrated,
    leftBehind: st.leftBehind,
    hibernated: st.hibernated,
    upgrades: st.upgrades,
    renamed: st.renamed,
    upgradeMismatch: st.upgradeMismatch,
    timeoutsAfterMove: st.timeoutsAfterMove,
    undeliverable: st.undeliverable,
    shortLivedWorkers: st.transients,
    retired: s.systems.workers.versionsRetired,
  };
  bench.result({ jobs: st.seq, lost: st.unacked.size, dups: st.dups, versions: st.version, migrated: st.migrated, retired: s.reload.retired });
});

bench.check('no job lost', (s) => s.reload.lost === 0);
bench.check('no job acknowledged twice', (s) => s.reload.dups === 0);
bench.check('no job undeliverable', (s) => s.reload.undeliverable === 0);
bench.check('every version served jobs', (s) => Array.from({ length: st.version }, (_, i) => i + 1).every((v) => s.reload.acksByVersion[v] > 0));
bench.check('running workers migrated to each new version', (s) => s.reload.migrated >= cfg.residents * cfg.reloads && s.reload.leftBehind === 0);
bench.check('hibernated workers were woken and migrated', (s) => cfg.reloads === 0 || s.reload.hibernated > 0);
bench.check('error.platform.upgrade carried the previous bindings', (s) => s.reload.upgrades >= s.reload.migrated && s.reload.upgradeMismatch === 0 && s.reload.renamed >= cfg.residents);
bench.check("each worker's count continues across versions", (s) => s.reload.gaps === 0);
bench.check('delayed sends survived the move', (s) => s.reload.timeoutsAfterMove > 0);
bench.check('old versions drained and were freed', (s) => s.reload.versions.every((v) => v.current || v.actors === 0) && s.reload.retired >= cfg.reloads - 1);
