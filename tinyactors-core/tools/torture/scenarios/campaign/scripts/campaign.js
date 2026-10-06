// Starts the campaign and follows its progress.
const P = bench.params;
globalThis.campaign = {
  config: {
    campaignID: 'spring-sale',
    total: P.total ?? bench.scaled(50000),
    concurrency: P.concurrency ?? 200,
    attemptTimeoutMs: P.attemptTimeoutMs ?? 3000,
    keyTTLMs: mail.keyTTLMs,
    maxUnsavedMs: P.maxUnsavedMs ?? 3000,
  },
  id: null,
  restarts: 0,
  last: null,
  start(resume) {
    const data = { ...this.config, resume: resume || null };
    this.id = bench.spawn('mailer', 'campaign', { data })[0];
  },
};
campaign.start();
bench.describeMetric('campaign', { title: 'Campaign progress', unit: 'recipients' });
bench.describeMetric('mail', { title: 'Mail processor', unit: 'events (cumulative)' });
bench.describeMetric('mailLatencyMs', { title: 'Mail processor latency', unit: 'milliseconds', format: 'ms' });
bench.on('campaign.progress', (c) => {
  campaign.last = c;
  bench.metric('campaign.sent', c.sent);
  bench.metric('campaign.claimed', c.cursor);
  bench.metric('campaign.inFlight', c.inflight.length);
  bench.metric('campaign.retrying', c.retries.length);
  bench.metric('campaign.failed', c.failed.length);
  bench.metric('campaign.duplicateAcks', c.duplicates);
});
bench.on('campaign.finished', (c) => {
  campaign.last = c;
  campaign.finished = c;
  bench.log(`campaign finished: ${c.sent} sent (${c.duplicates} acknowledged as duplicates), ${c.failed.length} failed, ${c.unknown.length} unknown of ${c.total}`);
});
bench.on('campaign.stale', (d) => bench.warn(`campaign refuses to resume: checkpoint is ${Math.round(d.checkpointAgeMs / 1000)} s old (keys are kept ${mail.keyTTLMs / 1000} s) — force or abort`));
bench.onFinish((summary) => {
  const c = campaign.finished || campaign.last;
  if (!c) return;
  bench.result({ total: c.total, sent: c.sent, failed: c.failed.length, unknown: c.unknown.length, duplicateAcks: c.duplicates, restarts: campaign.restarts, finished: campaign.finished ? 'yes' : 'no' });
});
bench.check('a finished campaign settled every recipient', () => {
  const c = campaign.finished;
  return !c || c.sent + c.failed.length + c.unknown.length === c.total;
});
bench.check('every recipient got at most one real delivery', () => bench.counter('deliveredTwice') === 0);
