// When each issue was filed and last changed, for issues from before the
// charts kept those times: taken from the ledger, which has always written
// the time of every change. At startup, each issue the index shows without
// a time gets the times of its first `filed` record and its latest record
// (`timestamps.backfill`); the issue sets only what it lacks, so a second
// start sends nothing. It writes no ledger record and tells no reporter.

// `support`: the host (lib/host.mjs); `ledger`: the Ledger. Answers how
// many issues were sent their times.
export async function backfillTimestamps({ support, ledger }) {
  const times = new Map();   // an issue's identifier -> {filed, updated}
  for (const { at, kind, key } of ledger.records) {
    if (!/^tinyactors:[a-z0-9._-]+:[0-9]+$/.test(key)) continue;   // decisions are keyed otherwise
    const known = times.get(key) ?? { filed: null, updated: null };
    if (kind === 'filed' && !known.filed) known.filed = at;
    known.updated = at;
    times.set(key, known);
  }
  const { data } = await support.request('tracker', 'projects.list', {});
  let sent = 0;
  for (const project of data?.projects ?? []) {
    const listed = await support.request('tracker', 'issues.list', { repo: project.name });
    if (listed.event !== 'issues') continue;
    for (const issue of listed.data.issues) {
      if (issue.filed && issue.updated) continue;
      const found = times.get(`tinyactors:${project.slug}:${issue.n}`);
      if (!found || (!found.filed && !found.updated)) continue;
      if (support.post(`issue.${project.repoID}.${issue.n}`, 'timestamps.backfill', found)) sent++;
    }
  }
  return sent;
}
