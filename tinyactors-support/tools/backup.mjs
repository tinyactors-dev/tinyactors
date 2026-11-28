// The backups of DATA_DIR (lib/backup.mjs), from the command line:
//
//   node tools/backup.mjs backup            a run, then the runs past BACKUP_KEEP (14) pruned
//   node tools/backup.mjs drill [run]       the newest run (or one) restored into a temporary directory, checked, deleted
//   node tools/backup.mjs restore <dir> [run]   a run restored into <dir> (an empty directory: DATA_DIR's layout)
//   node tools/backup.mjs list              the committed runs
//
// From the environment: DATA_DIR, BACKUP_S3_ENDPOINT (https://<account>.r2.cloudflarestorage.com),
// BACKUP_S3_BUCKET, BACKUP_S3_ACCESS_KEY_ID, BACKUP_S3_SECRET_ACCESS_KEY, BACKUP_S3_REGION (auto),
// BACKUP_KEY (32 bytes in base64: the AES-256-GCM key), BACKUP_KEEP. Without them it says which
// are missing and exits 0, so the timer of a VM with no bucket does nothing.

import { existsSync, readdirSync } from 'node:fs';
import { backup, fromEnvironment, restore, runs } from '../lib/backup.mjs';

const [command = 'backup', ...rest] = process.argv.slice(2);
const config = fromEnvironment();
if (config.missing) {
  console.log(`backup: not configured (missing ${config.missing.join(', ')}): nothing done`);
  process.exit(0);
}
const { bucket, key, keep } = config;
const data = process.env.DATA_DIR ?? new URL('../data/', import.meta.url).pathname;
switch (command) {
  case 'backup': {
    const m = await backup({ data, bucket, key, keep });
    console.log(`backup: run ${m.run}: ${m.items.length} objects, ${m.items.reduce((n, i) => n + i.raw.bytes, 0)} bytes; ` +
      `catalog ${JSON.stringify(m.items.find((i) => i.name === 'catalog.sqlite')?.facts?.rows ?? {})}`);
    break;
  }
  case 'drill': {
    const r = await restore({ bucket, key, run: rest[0] ?? null });
    console.log(`drill: run ${r.run} restored and checked: ${r.items} objects, ${r.bytes} bytes; rows ${JSON.stringify(r.rows)}`);
    break;
  }
  case 'restore': {
    const [to, run] = rest;
    if (!to || (existsSync(to) && readdirSync(to).length)) { console.error('restore: give an empty directory'); process.exit(2); }
    const r = await restore({ bucket, key, run: run ?? null, to });
    console.log(`restore: run ${r.run} into ${to}: ${r.items} objects, ${r.bytes} bytes`);
    break;
  }
  case 'list': console.log((await runs({ bucket })).committed.join('\n') || 'no committed run'); break;
  default: console.error(`backup: no command ${command}`); process.exit(2);
}
