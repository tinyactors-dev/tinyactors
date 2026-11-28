// Attachments' bytes: the gateway streams uploads in, the charts delete
// them and ask for signed links. Bytes never reach an actor, only keys do.
// A link is the server's /files/<key> with an expiry and an HMAC of both;
// the server checks it (verify) before it serves the file. An upload's link
// is /uploads/<id> with the slot's declared size and sha256, an expiry and
// an HMAC of them all (signUpload, verifyUpload): whoever holds it may PUT
// those bytes, and no others, until it expires; no token is needed.

import { createHmac, timingSafeEqual } from 'node:crypto';
import { mkdirSync, readFileSync, rmSync, writeFileSync, renameSync, existsSync } from 'node:fs';
import { dirname, join, resolve, sep } from 'node:path';

class Signed {
  // now: the wall clock in milliseconds (the host's, in tests a simulated one)
  constructor({ secret, base, now = Date.now }) { this.secret = secret; this.base = base; this.now = now; this.signed = 0; }
  signature(key, expires) { return createHmac('sha256', this.secret).update(`${key}\n${expires}`).digest('base64url'); }
  #matches(key, expires, signature) {
    if (!(Number(expires) > this.now() / 1000)) return false;
    const want = Buffer.from(this.signature(key, Number(expires))), got = Buffer.from(String(signature ?? ''));
    return want.length === got.length && timingSafeEqual(want, got);
  }
  // an upload's signed key: its slot, and the one file it takes (a download's keys never begin so)
  static uploadKey(uploadID, size, sha256) { return `upload:${uploadID}:${size}:${sha256}`; }
  signUpload({ uploadID, size, sha256, ttlSeconds }) {
    const expires = Math.floor(this.now() / 1000) + ttlSeconds;
    const query = new URLSearchParams({ size: String(size), sha256, expires: String(expires),
      signature: this.signature(Signed.uploadKey(uploadID, size, sha256), expires) });
    return `${this.base}/uploads/${uploadID}?${query}`;
  }
  // {size, sha256} the link was made for, or null (another link, or expired)
  verifyUpload(uploadID, { size, sha256, expires, signature }) {
    if (!/^[0-9]+$/.test(String(size)) || !/^[0-9a-f]{64}$/.test(String(sha256))) return null;
    return this.#matches(Signed.uploadKey(uploadID, Number(size), sha256), expires, signature)
      ? { size: Number(size), sha256 } : null;
  }
  sign(key, ttlSeconds = 300) {
    const expires = Math.floor(this.now() / 1000) + ttlSeconds;
    this.signed++;
    return { url: `${this.base}/files/${key.split('/').map(encodeURIComponent).join('/')}?expires=${expires}&signature=${this.signature(key, expires)}`,
             expiresIn: ttlSeconds };
  }
  verify(key, expires, signature) { return !String(key).startsWith('upload:') && this.#matches(key, expires, signature); }
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
