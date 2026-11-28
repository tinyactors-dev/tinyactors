// A stand-in S3 bucket for the backup tests: PUT (with If-None-Match: *),
// GET, DELETE and ListObjectsV2 over HTTP, path-style, each request's
// Signature Version 4 checked with the credentials it was made with.

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { signature } from '../lib/backup.mjs';

export async function s3({ accessKeyID = 'AKIDSTANDIN', secretAccessKey = 'standin-secret', bucket = 'backups', region = 'auto' } = {}) {
  const objects = new Map(), log = [];
  const server = createServer(async (req, res) => {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = Buffer.concat(chunks);
    const url = new URL(req.url, `http://${req.headers.host}`);
    const signed = /SignedHeaders=([^,]+)/.exec(req.headers.authorization ?? '')?.[1]?.split(';') ?? [];
    const headers = Object.fromEntries(signed.map((h) => [h, req.headers[h]]));
    const expected = signature({ method: req.method, url, headers, payloadHash: req.headers['x-amz-content-sha256'],
                                 accessKeyID, secretAccessKey, region, time: req.headers['x-amz-date'] ?? '' });
    const hash = createHash('sha256').update(body).digest('hex');
    log.push(`${req.method} ${url.pathname}${url.search}`);
    if (req.headers.authorization !== expected || hash !== req.headers['x-amz-content-sha256'] || !signed.includes('host')) {
      res.writeHead(403); res.end('<Error><Code>SignatureDoesNotMatch</Code></Error>'); return;
    }
    const [, name, ...rest] = url.pathname.split('/');
    const key = decodeURIComponent(rest.join('/'));
    if (name !== bucket) { res.writeHead(404); res.end('<Error><Code>NoSuchBucket</Code></Error>'); return; }
    if (req.method === 'GET' && url.searchParams.get('list-type') === '2') {
      const prefix = url.searchParams.get('prefix') ?? '';
      const keys = [...objects.keys()].filter((k) => k.startsWith(prefix)).sort();
      const start = Number(url.searchParams.get('continuation-token') ?? 0), page = keys.slice(start, start + 3);
      const more = start + 3 < keys.length;
      res.end(`<ListBucketResult>${page.map((k) => `<Contents><Key>${k}</Key></Contents>`).join('')}` +
              `<IsTruncated>${more}</IsTruncated>${more ? `<NextContinuationToken>${start + 3}</NextContinuationToken>` : ''}</ListBucketResult>`);
    } else if (req.method === 'PUT') {
      if (req.headers['if-none-match'] === '*' && objects.has(key)) { res.writeHead(412); res.end('<Error><Code>PreconditionFailed</Code></Error>'); return; }
      objects.set(key, body); res.end();
    } else if (req.method === 'GET') {
      if (!objects.has(key)) { res.writeHead(404); res.end('<Error><Code>NoSuchKey</Code></Error>'); return; }
      res.end(objects.get(key));
    } else if (req.method === 'DELETE') { objects.delete(key); res.writeHead(204); res.end(); }
    else { res.writeHead(405); res.end(); }
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { endpoint: `http://127.0.0.1:${server.address().port}`, bucket, accessKeyID, secretAccessKey, region, objects, log,
           close: () => new Promise((resolve) => server.close(resolve)) };
}
