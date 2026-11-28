// Backups of DATA_DIR to an S3 bucket (Cloudflare R2), and the drill that
// restores one: the smallest part of granary's backups that works here
// (granary: src/lib/ops/io/snapshot.worker.ts, src/lib/ops/backups/seal.ts,
// stores.ts and restore.ts).
//
// From granary, kept:
// - each database copied with `VACUUM INTO` on its own connection (a
//   consistent copy of a live WAL database), then `PRAGMA integrity_check`
//   and its tables' row counts, recorded in the manifest (snapshot.worker.ts);
// - zstd, then AES-256-GCM, the authenticated data naming the run and the
//   object, so an object moved to another run or name fails to decrypt
//   (seal.ts's chunk AAD);
// - the sha256 and size of the raw and the sealed bytes in the manifest,
//   checked again by the restore;
// - the manifest written last, `If-None-Match: *`, as the run's commit:
//   a run without one never happened, and a 412 means it is committed
//   already (stores.ts);
// - a restore drill: every object fetched, verified, decrypted, and each
//   database's integrity and row counts checked against the manifest
//   (restore.ts).
// Left out: the key hierarchy (a data key per run wrapped by a key
// encryption key, and key rotation), chunked streaming (DATA_DIR is a few
// megabytes; each object is sealed whole), the retention planner's
// calendar (the newest `keep` runs stay), Bun's S3 client (Node has none:
// requests are signed here, AWS Signature Version 4).
//
// What a run holds: catalog.sqlite (the issues and their ledger),
// clients.sqlite (the OAuth clients), store.sqlite with host.snapshot (the
// actors; the snapshot is read before and after the store's copy and the
// run retries if it changed, since the server deletes blobs right after it
// writes a snapshot), file-secret, and files/ (the attachments' bytes).

import { createCipheriv, createDecipheriv, createHash, createHmac, randomBytes } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { zstdCompressSync, zstdDecompressSync } from 'node:zlib';

export const FORMAT = 'tinyactors-support-backup/1';
const DATABASES = ['catalog.sqlite', 'clients.sqlite', 'store.sqlite'];
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

// --- S3 (R2): AWS Signature Version 4 ----------------------------------------------

// RFC 3986 encoding, as Signature Version 4 asks (`/` kept in a path).
const encode = (text, slash = false) => {
  const encoded = encodeURIComponent(text).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
  return slash ? encoded.replace(/%2F/g, '/') : encoded;
};

// The Authorization header of a request: method, URL, its headers (lower
// case names, host included), the payload's sha256, the credentials and
// the time (YYYYMMDDTHHMMSSZ).
export function signature({ method, url, headers, payloadHash, accessKeyID, secretAccessKey, region, service = 's3', time }) {
  const day = time.slice(0, 8), scope = `${day}/${region}/${service}/aws4_request`;
  const names = Object.keys(headers).map((h) => h.toLowerCase()).sort();
  const lower = Object.fromEntries(Object.entries(headers).map(([k, v]) => [k.toLowerCase(), String(v).trim()]));
  const query = [...url.searchParams].map(([k, v]) => [encode(k), encode(v)]).sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : 1))
    .map(([k, v]) => `${k}=${v}`).join('&');
  const path = url.pathname.split('/').map((part) => encode(decodeURIComponent(part))).join('/');
  const canonical = [method, path, query, names.map((h) => `${h}:${lower[h]}\n`).join(''), names.join(';'), payloadHash].join('\n');
  const toSign = ['AWS4-HMAC-SHA256', time, scope, sha256(canonical)].join('\n');
  const hmac = (key, text) => createHmac('sha256', key).update(text).digest();
  const key = ['aws4_request'].reduce((k, part) => hmac(k, part),
    hmac(hmac(hmac(`AWS4${secretAccessKey}`, day), region), service));
  const signed = createHmac('sha256', key).update(toSign).digest('hex');
  return `AWS4-HMAC-SHA256 Credential=${accessKeyID}/${scope}, SignedHeaders=${names.join(';')}, Signature=${signed}`;
}

// A bucket, path-style (R2: https://<account>.r2.cloudflarestorage.com, region auto).
export class Bucket {
  constructor({ endpoint, bucket, accessKeyID, secretAccessKey, region = 'auto', fetch = globalThis.fetch, now = Date.now }) {
    Object.assign(this, { endpoint: endpoint.replace(/\/$/, ''), bucket, accessKeyID, secretAccessKey, region, fetch, now });
  }

  async #request(method, key, { body = null, headers = {}, query = {} } = {}) {
    const url = new URL(`${this.endpoint}/${encode(this.bucket)}/${encode(key, true)}`);
    for (const [k, v] of Object.entries(query)) url.searchParams.set(k, v);
    const time = new Date(this.now()).toISOString().replace(/[-:]|\.\d{3}/g, '');
    const payloadHash = sha256(body ?? Buffer.alloc(0));
    const all = { ...headers, host: url.host, 'x-amz-content-sha256': payloadHash, 'x-amz-date': time };
    const authorization = signature({ method, url, headers: all, payloadHash, accessKeyID: this.accessKeyID,
                                      secretAccessKey: this.secretAccessKey, region: this.region, time });
    const { host, ...sent } = all;
    return this.fetch(url, { method, body, headers: { ...sent, authorization } });
  }

  async #ok(response, what) {
    if (response.ok) return response;
    throw new Error(`${what}: ${response.status} ${(await response.text()).slice(0, 200)}`);
  }

  async put(key, body) { await this.#ok(await this.#request('PUT', key, { body }), `PUT ${key}`); }

  // Create once: false when the key is there already (412).
  async create(key, body) {
    const response = await this.#request('PUT', key, { body, headers: { 'if-none-match': '*' } });
    if (response.status === 412) return false;
    await this.#ok(response, `PUT (If-None-Match) ${key}`);
    return true;
  }

  async get(key) { return Buffer.from(await (await this.#ok(await this.#request('GET', key), `GET ${key}`)).arrayBuffer()); }
  async delete(key) { await this.#ok(await this.#request('DELETE', key), `DELETE ${key}`); }

  async list(prefix) {
    const keys = [];
    let token = null;
    do {
      const query = { 'list-type': '2', prefix, ...(token ? { 'continuation-token': token } : {}) };
      const xml = await (await this.#ok(await this.#request('GET', '', { query }), `LIST ${prefix}`)).text();
      for (const [, key] of xml.matchAll(/<Key>([^<]*)<\/Key>/g)) keys.push(key.replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>'));
      token = /<IsTruncated>true<\/IsTruncated>/.test(xml) ? /<NextContinuationToken>([^<]*)</.exec(xml)?.[1] : null;
    } while (token);
    return keys;
  }
}

// --- sealing --------------------------------------------------------------------------

const aad = (run, name) => Buffer.from(`${FORMAT}|${run}|${name}`);

export function seal(key, run, name, raw) {
  const iv = randomBytes(12), cipher = createCipheriv('aes-256-gcm', key, iv);
  cipher.setAAD(aad(run, name));
  const body = Buffer.concat([cipher.update(zstdCompressSync(raw)), cipher.final()]);
  return Buffer.concat([iv, body, cipher.getAuthTag()]);
}

export function unseal(key, run, name, sealed) {
  const decipher = createDecipheriv('aes-256-gcm', key, sealed.subarray(0, 12));
  decipher.setAAD(aad(run, name));
  decipher.setAuthTag(sealed.subarray(sealed.length - 16));
  let compressed;
  try { compressed = Buffer.concat([decipher.update(sealed.subarray(12, sealed.length - 16)), decipher.final()]); }
  catch { throw new Error(`${name}: it does not decrypt (another key, or a damaged or moved object)`); }
  return zstdDecompressSync(compressed);
}

export function backupKey(text) {
  const key = Buffer.from(String(text ?? ''), 'base64');
  if (key.length !== 32) throw new Error('BACKUP_KEY is not 32 bytes in base64');
  return key;
}

// --- a run ------------------------------------------------------------------------------

// A database's integrity and its tables' row counts (snapshot.worker.ts's facts).
export function facts(path) {
  const db = new DatabaseSync(path, { readOnly: true });
  try {
    const check = db.prepare('PRAGMA integrity_check').all().map((r) => r.integrity_check);
    if (check.length !== 1 || check[0] !== 'ok') throw new Error(`${path}: integrity_check: ${check.slice(0, 5).join('; ')}`);
    const tables = db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' AND sql NOT LIKE 'CREATE VIRTUAL%' AND name NOT LIKE '%_fts_%' ORDER BY name").all();
    return { rows: Object.fromEntries(tables.map(({ name }) => [name, db.prepare(`SELECT count(*) AS n FROM "${name.replaceAll('"', '""')}"`).get().n])) };
  } finally { db.close(); }
}

function copyDatabase(from, to) {
  const db = new DatabaseSync(from);
  try { db.exec('PRAGMA busy_timeout = 5000'); db.prepare('VACUUM INTO ?').run(to); } finally { db.close(); }
}

// The files of a run, read from `data`: [{name, raw, facts?}].
export function collect(data) {
  const work = mkdtempSync(join(tmpdir(), 'support-backup-'));
  try {
    const items = [];
    for (const name of DATABASES.filter((d) => d !== 'store.sqlite')) {
      if (!existsSync(join(data, name))) continue;
      copyDatabase(join(data, name), join(work, name));
      items.push({ name, raw: readFileSync(join(work, name)), facts: facts(join(work, name)) });
    }
    // the store with a snapshot it holds every blob of: a snapshot written
    // during the store's copy (and the deletes after it) means another try
    for (let attempt = 0; ; attempt++) {
      const snapshot = existsSync(join(data, 'host.snapshot')) ? readFileSync(join(data, 'host.snapshot')) : null;
      if (existsSync(join(data, 'store.sqlite'))) copyDatabase(join(data, 'store.sqlite'), join(work, 'store.sqlite'));
      const again = existsSync(join(data, 'host.snapshot')) ? readFileSync(join(data, 'host.snapshot')) : null;
      if ((snapshot === null && again === null) || (snapshot && again && snapshot.equals(again))) {
        if (snapshot) items.push({ name: 'host.snapshot', raw: snapshot });
        if (existsSync(join(work, 'store.sqlite'))) items.push({ name: 'store.sqlite', raw: readFileSync(join(work, 'store.sqlite')), facts: facts(join(work, 'store.sqlite')) });
        break;
      }
      if (attempt >= 5) throw new Error('the snapshot kept changing while the store was copied');
    }
    if (existsSync(join(data, 'file-secret'))) items.push({ name: 'file-secret', raw: readFileSync(join(data, 'file-secret')) });
    const files = join(data, 'files');
    const walk = (dir) => readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
      e.isDirectory() ? walk(join(dir, e.name)) : e.isFile() ? [join(dir, e.name)] : []);
    if (existsSync(files)) for (const path of walk(files).sort()) items.push({ name: relative(data, path), raw: readFileSync(path) });
    return items;
  } finally { rmSync(work, { recursive: true, force: true }); }
}

const runID = (ms) => new Date(ms).toISOString().replace(/[-:]|\.\d{3}/g, '');

// One run: every item sealed and put, then the manifest, created once.
// Then the runs past `keep` (and objects of runs never committed, older
// than the newest committed one) are deleted. Answers the manifest.
export async function backup({ data, bucket, key, prefix = 'backups/', keep = 14, now = Date.now }) {
  const run = runID(now());
  const items = collect(data);
  const entries = [];
  for (const [index, item] of items.entries()) {
    const sealed = seal(key, run, item.name, item.raw);
    const object = `${prefix}${run}/${String(index).padStart(4, '0')}.bin`;
    await bucket.put(object, sealed);
    entries.push({ name: item.name, object, raw: { bytes: item.raw.length, sha256: sha256(item.raw) },
                   sealed: { bytes: sealed.length, sha256: sha256(sealed) }, ...(item.facts ? { facts: item.facts } : {}) });
  }
  const manifest = { format: FORMAT, run, at: new Date(now()).toISOString(), items: entries };
  if (!await bucket.create(`${prefix}${run}/manifest.json`, Buffer.from(JSON.stringify(manifest, null, 1)))) {
    throw new Error(`run ${run} is committed already`);
  }
  await prune({ bucket, prefix, keep });
  return manifest;
}

export async function runs({ bucket, prefix = 'backups/' }) {
  const keys = await bucket.list(prefix);
  const committed = keys.filter((k) => k.endsWith('/manifest.json')).map((k) => k.slice(prefix.length).split('/')[0]).sort();
  return { keys, committed };
}

export async function prune({ bucket, prefix = 'backups/', keep = 14 }) {
  const { keys, committed } = await runs({ bucket, prefix });
  const kept = new Set(committed.slice(-keep)), newest = committed.at(-1);
  const gone = keys.filter((k) => {
    const run = k.slice(prefix.length).split('/')[0];
    return !kept.has(run) && (committed.includes(run) || (newest && run < newest));
  });
  for (const k of gone) await bucket.delete(k);
  return gone.length;
}

// A restore: the newest committed run (or `run`), every object verified
// and decrypted, every database's integrity and row counts checked; into
// `to` (DATA_DIR's layout), or into a directory deleted after (the drill).
export async function restore({ bucket, key, prefix = 'backups/', run = null, to = null }) {
  const { committed } = await runs({ bucket, prefix });
  const chosen = run ?? committed.at(-1);
  if (!chosen || !committed.includes(chosen)) throw new Error(run ? `no committed run ${run}` : 'no committed run');
  const manifest = JSON.parse(await bucket.get(`${prefix}${chosen}/manifest.json`));
  if (manifest.format !== FORMAT) throw new Error(`run ${chosen} is ${manifest.format}, not ${FORMAT}`);
  const target = to ?? mkdtempSync(join(tmpdir(), 'support-drill-'));
  try {
    for (const item of manifest.items) {
      const sealed = await bucket.get(item.object);
      if (sealed.length !== item.sealed.bytes || sha256(sealed) !== item.sealed.sha256) throw new Error(`${item.name}: the object is not the one the manifest names`);
      const raw = unseal(key, chosen, item.name, sealed);
      if (raw.length !== item.raw.bytes || sha256(raw) !== item.raw.sha256) throw new Error(`${item.name}: the restored bytes are not the ones backed up`);
      if (item.name.includes('..')) throw new Error(`${item.name}: not a name under the data directory`);
      const path = join(target, item.name);
      mkdirSync(dirname(path), { recursive: true });
      writeFileSync(path, raw, { mode: 0o600 });
      if (item.facts) {
        const found = facts(path);
        if (JSON.stringify(found.rows) !== JSON.stringify(item.facts.rows)) throw new Error(`${item.name}: row counts ${JSON.stringify(found.rows)}, the manifest says ${JSON.stringify(item.facts.rows)}`);
      }
    }
    return { run: chosen, items: manifest.items.length, bytes: manifest.items.reduce((n, i) => n + i.raw.bytes, 0),
             rows: Object.fromEntries(manifest.items.filter((i) => i.facts).map((i) => [i.name, i.facts.rows])) };
  } finally { if (!to) rmSync(target, { recursive: true, force: true }); }
}

// The bucket and the key from the environment, or null when they are not all set.
export function fromEnvironment(env = process.env) {
  const names = ['BACKUP_S3_ENDPOINT', 'BACKUP_S3_BUCKET', 'BACKUP_S3_ACCESS_KEY_ID', 'BACKUP_S3_SECRET_ACCESS_KEY', 'BACKUP_KEY'];
  const missing = names.filter((n) => !env[n]);
  if (missing.length) return { missing };
  return { bucket: new Bucket({ endpoint: env.BACKUP_S3_ENDPOINT, bucket: env.BACKUP_S3_BUCKET, accessKeyID: env.BACKUP_S3_ACCESS_KEY_ID,
                                secretAccessKey: env.BACKUP_S3_SECRET_ACCESS_KEY, region: env.BACKUP_S3_REGION || 'auto' }),
           key: backupKey(env.BACKUP_KEY), keep: Number(env.BACKUP_KEEP || 14) };
}

