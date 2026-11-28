// mcp.tinyactors.dev: `node server.mjs`. Configuration from the
// environment (the deploy writes it to /etc/tinyactors-support.env):
//
//   PORT                  8000 (exe's proxy forwards to it)
//   PUBLIC_URL            the URL clients reach, https://mcp.tinyactors.dev
//   DATA_DIR              the snapshot, the store, the catalog (catalog.sqlite: every
//                         issue and its ledger), the clients, the files
//   REBUILD               1: start the actors from the catalog, the snapshot and the
//                         store set aside (`mise run rebuild`); people sign in again
//   GITHUB_APP_ID, GITHUB_APP_CLIENT_ID, GITHUB_APP_CLIENT_SECRET, GITHUB_APP_PRIVATE_KEY
//                         the GitHub App; without all four, the stand-in GitHub
//                         (fake repositories) and the stand-in login
//   GITHUB_ORG            tinyactors-dev
//   TYPESAFE_API_KEY      Jev, for duplicates; without it reports that match no
//                         issue exactly are filed as new
//   FILE_SECRET           signs download links; made and kept in DATA_DIR when absent
//   TINYACTORS_CLI        the tinyactors command, to summarise attached snapshots

import { existsSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { Support } from './lib/host.mjs';
import { Gateway } from './lib/gateway.mjs';
import { Content } from './lib/content.mjs';
import { serve } from './lib/http.mjs';
import { DiskBlobs } from './lib/processors/blobs.mjs';
import { Catalog } from './lib/processors/catalog.mjs';
import { Scanner } from './lib/processors/scan.mjs';
import { Jev } from './lib/processors/jev.mjs';
import { migrateToCatalog } from './lib/migrate.mjs';
import { GitHubApp } from './lib/processors/github.mjs';
import { FakeGitHub } from './lib/processors/standins.mjs';

const env = process.env;
const log = (line) => console.log(`${new Date().toISOString()} ${line}`);
const port = Number(env.PORT ?? 8000);
const base = env.PUBLIC_URL ?? `http://localhost:${port}`;
const data = env.DATA_DIR ?? new URL('./data/', import.meta.url).pathname;
mkdirSync(data, { recursive: true });

function fileSecret() {
  if (env.FILE_SECRET) return env.FILE_SECRET;
  const path = join(data, 'file-secret');
  if (!existsSync(path)) writeFileSync(path, randomBytes(32).toString('base64url'), { mode: 0o600 });
  return readFileSync(path, 'utf8');
}

const app = ['GITHUB_APP_ID', 'GITHUB_APP_CLIENT_ID', 'GITHUB_APP_CLIENT_SECRET', 'GITHUB_APP_PRIVATE_KEY'].every((k) => env[k]);
const github = app
  ? new GitHubApp({ appID: env.GITHUB_APP_ID, clientID: env.GITHUB_APP_CLIENT_ID, clientSecret: env.GITHUB_APP_CLIENT_SECRET,
                    privateKey: env.GITHUB_APP_PRIVATE_KEY.replace(/\\n/g, '\n'), org: env.GITHUB_ORG ?? 'tinyactors-dev', base })
  : new FakeGitHub();
const blobs = new DiskBlobs({ directory: join(data, 'files'), secret: fileSecret(), base });
const catalog = new Catalog(join(data, 'catalog.sqlite'));
const support = new Support({
  data, log, rebuild: env.REBUILD === '1',
  processors: { github: github.processor, catalog: catalog.processor, decide: new Jev({ key: env.TYPESAFE_API_KEY }).processor,
                blobs: blobs.processor, scan: new Scanner(blobs, env.TINYACTORS_CLI).processor },
});
const content = new Content({
  skills: new URL('./skills/', import.meta.url).pathname,
  docs: [{ directory: new URL('./content/', import.meta.url).pathname, prefix: 'docs' }],
});
const gateway = new Gateway({ support, base, github: app ? github : null, blobs, content, catalog });
// the first start with a catalog: the old actors read before they move to the new charts
await migrateToCatalog({ support, catalog, data, log });
support.upgrade();
// the repositories' names, for the reads, as the tracker has them now
const { data: listed } = await support.request('tracker', 'projects.list', {});
if (listed?.projects) catalog.projects(listed.projects);
const server = await serve({ support, gateway, port, log, clientsPath: join(data, 'clients.sqlite'),
                             standinUsers: app ? null : Object.values(new FakeGitHub().users).map((u) => u.login) });
log(`tinyactors-support on ${base} (port ${port}): ${app ? 'the GitHub App' : 'the stand-in GitHub and login'}, ` +
    `${env.TYPESAFE_API_KEY ? 'Jev' : 'no Jev'}, ${content.skills.length} skills, ${content.docs.length} documents, ` +
    `${support.restored ? 'restored' : env.REBUILD === '1' ? 'rebuilt from the catalog' : 'a new host'}, ` +
    `${catalog.issueCount()} issues in the catalog`);

let stopping = false;
async function stop(signal) {
  if (stopping) return;
  stopping = true;
  log(`${signal}: stopping`);
  server.close();
  await support.close();
  catalog.close();
  log('stopped: the snapshot is on disk');
  process.exit(0);
}
process.on('SIGTERM', () => stop('SIGTERM'));
process.on('SIGINT', () => stop('SIGINT'));
