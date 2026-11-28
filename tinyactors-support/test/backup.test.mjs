// The backups: AWS Signature Version 4 against AWS's own example, a run
// and its drill against a stand-in S3 bucket, a restore a host starts
// from, and what the restore refuses (an object moved, damaged, another
// key), the manifest created once, and the runs pruned.

import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { Bucket, backup, prune, restore, runs, signature } from '../lib/backup.mjs';
import { Catalog } from '../lib/processors/catalog.mjs';
import { FakeGitHub, FakeJev } from '../lib/processors/standins.mjs';
import { MemoryBlobs } from '../lib/processors/blobs.mjs';
import { BASE, stack } from './stack.mjs';
import { s3 } from './s3.mjs';

const report = { repo: 'tinyactors-dev/tinyactors-napi', component: 'lib', kind: 'tooling', minutes: 9, commit: 'c2',
                 title: 'the store loses a blob', body: 'a crash between a snapshot and a delete' };

describe('backups', () => {
  it('signs as AWS\'s Signature Version 4 example does (GET Object)', () => {
    // https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html, "Example: GET Object"
    const empty = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855';
    const authorization = signature({
      method: 'GET', url: new URL('https://examplebucket.s3.amazonaws.com/test.txt'), payloadHash: empty,
      headers: { host: 'examplebucket.s3.amazonaws.com', range: 'bytes=0-9', 'x-amz-content-sha256': empty, 'x-amz-date': '20130524T000000Z' },
      accessKeyID: 'AKIAIOSFODNN7EXAMPLE', secretAccessKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY', region: 'us-east-1',
      time: '20130524T000000Z' });
    assert.equal(authorization, 'AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, ' +
      'SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41');
  });

  it('backs up DATA_DIR sealed, restores it whole, and a host starts from the restore', async () => {
    const server = await s3();
    const bucket = new Bucket(server), key = randomBytes(32);
    const data = mkdtempSync(join(tmpdir(), 'support-'));
    const shared = () => ({ github: new FakeGitHub(), jev: new FakeJev(), blobs: new MemoryBlobs({ secret: 'test', base: BASE }) });
    let s = await stack({ data, shared: { ...shared(), catalog: new Catalog(join(data, 'catalog.sqlite')) } });
    const ann = (await s.signIn('code-ann')).access_token;
    const filed = await s.tool(ann, 'report_issue', report);
    await s.support.close();   // its snapshot on disk
    mkdirSync(join(data, 'files', 'tinyactors-napi'), { recursive: true });
    writeFileSync(join(data, 'files', 'tinyactors-napi', 'a.log'), 'a log line\n');
    writeFileSync(join(data, 'file-secret'), 'not a real secret');
    const manifest = await backup({ data, bucket, key, now: () => Date.UTC(2026, 10, 28, 3, 17) });
    assert.deepEqual(manifest.items.map((i) => i.name).sort(),
                     ['catalog.sqlite', 'file-secret', 'files/tinyactors-napi/a.log', 'host.snapshot', 'store.sqlite']);
    assert.equal(manifest.items.find((i) => i.name === 'catalog.sqlite').facts.rows.issues, 1);
    for (const [k, bytes] of server.objects) {
      if (!k.endsWith('manifest.json')) assert.ok(!bytes.includes('the store loses a blob'), `${k} is sealed`);
    }
    // the drill, then a restore a host starts from: the issue and its actor are there
    assert.equal((await restore({ bucket, key })).run, '20261128T031700Z');
    const to = mkdtempSync(join(tmpdir(), 'support-restored-'));
    await restore({ bucket, key, to });
    s = await stack({ data: to, shared: { ...shared(), catalog: new Catalog(join(to, 'catalog.sqlite')) } });
    assert.equal(s.support.restored, true);
    assert.equal((await s.tool(null, 'get_issue', { id: filed.id })).title, report.title);
    assert.equal((await s.tool(ann, 'report_issue', report)).answer, 'counted', 'the issue\'s actor, and the login, came back');
    await s.support.close();
    // the manifest is created once
    await assert.rejects(backup({ data, bucket, key, now: () => Date.UTC(2026, 10, 28, 3, 17) }), /committed already/);
    await server.close();
  });

  it('refuses an object moved, damaged or sealed with another key; keeps the newest runs', async () => {
    const server = await s3();
    const bucket = new Bucket(server), key = randomBytes(32);
    const data = mkdtempSync(join(tmpdir(), 'support-'));
    const catalog = new Catalog(join(data, 'catalog.sqlite'));
    catalog.append('filed', 'tinyactors:tinyactors:1', { title: 'one', component: 'docs', kind: 'docs', body: '' });
    catalog.close();
    writeFileSync(join(data, 'file-secret'), 'x');
    let t = Date.UTC(2026, 10, 1);
    for (let day = 0; day < 4; day++) await backup({ data, bucket, key, keep: 3, now: () => t + day * 86_400_000 });
    assert.deepEqual((await runs({ bucket })).committed, ['20261102T000000Z', '20261103T000000Z', '20261104T000000Z']);
    // a run that never committed (no manifest), older than the newest: pruned
    await bucket.put('backups/20261102T120000Z/0000.bin', Buffer.from('half a run'));
    assert.equal(await prune({ bucket, keep: 3 }), 1);
    await assert.rejects(restore({ bucket, key: randomBytes(32) }), /does not decrypt/);
    const objects = [...server.objects.keys()].filter((k) => k.startsWith('backups/20261104T000000Z/') && k.endsWith('.bin')).sort();
    const [first, second] = objects.map((k) => server.objects.get(k));
    server.objects.set(objects[0], second); server.objects.set(objects[1], first);   // swapped
    await assert.rejects(restore({ bucket, key }), /not the one the manifest names/);
    server.objects.set(objects[0], first); server.objects.set(objects[1], second);
    first[20] ^= 1;
    await assert.rejects(restore({ bucket, key }), /not the one the manifest names/);
    first[20] ^= 1;
    assert.equal((await restore({ bucket, key })).items, 2);
    await assert.rejects(new Bucket({ ...server, secretAccessKey: 'wrong' }).list('backups/'), /403/);
    await server.close();
  });
});
