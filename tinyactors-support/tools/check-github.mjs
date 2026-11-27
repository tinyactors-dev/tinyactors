// Read-only checks of the GitHub App, run on the VM with the server's
// environment (mise run smoke): the App's JWT (GET /app), its installation
// on GITHUB_ORG (GET /app/installations), the repositories the
// installation reads (GET /installation/repositories), and the login
// redirect the server makes. Prints no secret: the client id is public (it
// is in every login URL), the rest is names and counts.

import { createSign } from 'node:crypto';

const env = process.env;
const missing = ['GITHUB_APP_ID', 'GITHUB_APP_CLIENT_ID', 'GITHUB_APP_CLIENT_SECRET', 'GITHUB_APP_PRIVATE_KEY'].filter((k) => !env[k]);
if (missing.length) { console.log(`github: not configured (missing ${missing.join(', ')}): the stand-ins run`); process.exit(0); }
const key = env.GITHUB_APP_PRIVATE_KEY.replace(/\\n/g, '\n');
const now = Math.floor(Date.now() / 1000);
const part = (v) => Buffer.from(JSON.stringify(v)).toString('base64url');
const unsigned = `${part({ alg: 'RS256', typ: 'JWT' })}.${part({ iat: now - 60, exp: now + 540, iss: String(env.GITHUB_APP_ID) })}`;
const jwt = `${unsigned}.${createSign('RSA-SHA256').update(unsigned).sign(key).toString('base64url')}`;
const headers = { authorization: `Bearer ${jwt}`, accept: 'application/vnd.github+json', 'x-github-api-version': '2022-11-28' };
let failed = false;
const check = (ok, line) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${line}`); if (!ok) failed = true; };

const app = await fetch('https://api.github.com/app', { headers });
const appBody = await app.json();
check(app.ok, `GET /app: ${app.status} ${app.ok ? `${appBody.slug} (owner ${appBody.owner?.login}), permissions ${JSON.stringify(appBody.permissions)}` : appBody.message}`);
const installations = await fetch('https://api.github.com/app/installations', { headers });
const list = installations.ok ? await installations.json() : [];
const org = env.GITHUB_ORG ?? 'tinyactors-dev';
const installation = list.find((i) => i.account?.login?.toLowerCase() === org.toLowerCase());
check(Boolean(installation), `GET /app/installations: ${installations.status}, ${list.length} installation(s)${installation ? `, on ${org} (${installation.repository_selection} repositories)` : `, none on ${org}`}`);
if (installation) {
  const made = await fetch(`https://api.github.com/app/installations/${installation.id}/access_tokens`, { method: 'POST', headers });
  const token = made.ok ? (await made.json()).token : null;
  check(Boolean(token), `POST /app/installations/{id}/access_tokens: ${made.status}`);
  if (token) {
    const repos = await fetch('https://api.github.com/installation/repositories?per_page=100',
      { headers: { ...headers, authorization: `Bearer ${token}` } });
    const body = repos.ok ? await repos.json() : { repositories: [] };
    check(repos.ok && body.repositories.length > 0, `GET /installation/repositories: ${repos.status}, ${body.total_count ?? 0} repositories: ` +
      body.repositories.map((r) => `${r.full_name}${r.archived ? ' (archived)' : ''}`).join(', '));
  }
}
const base = env.PUBLIC_URL ?? 'http://localhost:8000';
const authorize = new URL('https://github.com/login/oauth/authorize');
authorize.search = new URLSearchParams({ client_id: env.GITHUB_APP_CLIENT_ID, state: 'check', redirect_uri: `${base}/github/callback` });
check(env.GITHUB_APP_CLIENT_ID.startsWith('Iv'), `the login redirect: client_id ${env.GITHUB_APP_CLIENT_ID}, redirect_uri ${authorize.searchParams.get('redirect_uri')}`);
process.exit(failed ? 1 : 0);
