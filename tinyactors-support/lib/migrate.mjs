// The move to the catalog, once, on the first start of the code that has
// one: the issues of before were their actors' state, with the ledger
// (ledger.sqlite) as their journal, and the journal did not record
// everything (a report's minutes and reporter, the files' descriptions).
//
// The host is restored on the old charts first (the new version waits for
// `support.upgrade()`), so the old issue actors still hold every field:
// their variables are read (`actor.context`), the old ledger is replayed
// into the catalog with its times, and each issue gets one `migrated`
// record of what its actor held. The old ledger is then renamed
// (ledger.sqlite.imported, never deleted). The catalog's `migrated` mark
// makes it once: a second start, or a crash after the catalog's
// transaction and before the rename, does nothing again.

import { existsSync, renameSync } from 'node:fs';
import { StateError } from '@tinyactors/node';

const FIELDS = ['repoID', 'title', 'component', 'kind', 'body', 'count', 'minutes', 'reporters', 'workaround', 'reason',
                'fix', 'state', 'attachments', 'purged', 'filed', 'updated'];

// The old issue actors' variables, as `migrated` records. A hibernated
// actor answers `state` to a context: an event it ignores wakes it.
export async function oldIssues(support) {
  const out = [];
  for (const actor of await support.host.actors({ chart: 'issue' })) {
    let variables;
    for (let attempt = 0; ; attempt++) {
      try { ({ variables } = await actor.context()); break; }
      catch (error) {
        if (!(error instanceof StateError) || attempt >= 3) throw error;
        actor.send('catalog.migrating');
        await support.host.idle();
      }
    }
    if (variables.deletion) continue;   // being deleted: its ledger says so, or it stays an issue
    const record = Object.fromEntries(FIELDS.map((f) => [f, variables[f] ?? null]));
    record.possibleDuplicateOf = variables.possible ?? null;
    out.push({ id: `tinyactors:${variables.slug}:${variables.n}`, ...record });
  }
  return out;
}

// Answers what it did: null when there was nothing to migrate.
export async function migrateToCatalog({ support, catalog, data, log = () => {} }) {
  const legacy = `${data}/ledger.sqlite`;
  if (catalog.meta('migrated') || (!existsSync(legacy) && !support.restored)) return null;
  const { data: listed } = await support.request('tracker', 'projects.list', {});
  catalog.projects(listed?.projects ?? []);
  const issues = support.restored ? await oldIssues(support) : [];
  catalog.migrate({ legacy, issues });
  if (existsSync(legacy)) renameSync(legacy, `${legacy}.imported`);
  for (const suffix of ['-wal', '-shm']) if (existsSync(legacy + suffix)) renameSync(legacy + suffix, `${legacy}.imported${suffix}`);
  const done = { records: catalog.records.length, issues: issues.length, catalog: catalog.issueCount() };
  log(`migrated to the catalog: ${done.records} ledger records, ${done.issues} issues from their actors, ${done.catalog} in the catalog`);
  return done;
}
