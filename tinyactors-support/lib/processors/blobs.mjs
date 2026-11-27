// Attachments' bytes: the gateway streams uploads in, the charts delete
// them and ask for signed links. Bytes never reach an actor, only keys do.
// A link is the server's /files/<key> with an expiry and an HMAC of both;
// the server checks it (verify) before it serves the file.

import { createHmac, timingSafeEqual } from 'node:crypto';
import { mkdirSync, readFileSync, rmSync, writeFileSync, renameSync, existsSync } from 'node:fs';
import { dirname, join, resolve, sep } from 'node:path';

class Signed {
  constructor({ secret, base }) { this.secret = secret; this.base = base; this.signed = 0; }
  signature(key, expires) { return createHmac('sha256', this.secret).update(`${key}\n${expires}`).digest('base64url'); }
  sign(key, ttlSeconds = 300) {
    const expires = Math.floor(Date.now() / 1000) + ttlSeconds;
    this.signed++;
    return { url: `${this.base}/files/${key.split('/').map(encodeURIComponent).join('/')}?expires=${expires}&signature=${this.signature(key, expires)}`,
             expiresIn: ttlSeconds };
  }
  verify(key, expires, signature) {
    if (!(Number(expires) > Date.now() / 1000)) return false;
    const want = Buffer.from(this.signature(key, Number(expires))), got = Buffer.from(String(signature ?? ''));
    return want.length === got.length && timingSafeEqual(want, got);
  }
  processor = (event, data) => {
    switch (event) {
      case 'blob.delete': this.delete(data.key); return { event: 'blob.deleted', data: { key: data.key } };
      case 'blob.sign': return this.get(data.key) ? { event: 'blob.signed', data: this.sign(data.key) } : null;
    }
    return null;
  };
}

export class MemoryBlobs extends Signed {
  constructor(options) { super(options); this.objects = new Map(); }
  put(key, bytes) { this.objects.set(key, Buffer.from(bytes)); }
  get(key) { return this.objects.get(key); }
  delete(key) { this.objects.delete(key); }
  keys() { return [...this.objects.keys()]; }
}

// On the VM's disk, under one directory; a key never leaves it.
export class DiskBlobs extends Signed {
  constructor({ directory, ...options }) { super(options); this.directory = resolve(directory); mkdirSync(this.directory, { recursive: true }); }
  #path(key) {
    const path = resolve(join(this.directory, key));
    if (!path.startsWith(this.directory + sep)) throw new Error(`a key outside the store: ${key}`);
    return path;
  }
  put(key, bytes) {
    const path = this.#path(key);
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(`${path}.partial`, bytes);
    renameSync(`${path}.partial`, path);
  }
  get(key) { const path = this.#path(key); return existsSync(path) ? readFileSync(path) : undefined; }
  delete(key) { rmSync(this.#path(key), { force: true }); }
}
