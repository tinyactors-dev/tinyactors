// The scan: what an attachment's stored bytes are (never what they say they
// are), and whether they may be attached. Logs are redacted into a copy
// that replaces the original; archives are listed, never extracted;
// snapshots of tinyactors are summarised by `tinyactors inspect` when the
// command is installed (TINYACTORS_CLI).

import { execFileSync } from 'node:child_process';
export class Scanner {
  constructor(blobs, command) { this.blobs = blobs; this.command = command; this.unavailable = 0; }

  processor = async (event, data) => {
    if (event !== 'scan') return null;
    if (this.unavailable > 0) { this.unavailable--; return { event: 'scan.unavailable', data: {} }; }
    const bytes = this.blobs.get(data.key);
    if (!bytes) return null;
    const type = sniff(bytes);
    const answer = (fields) => ({ event: 'scanned', data: { type, key: data.key, verdict: 'ok', ...fields } });
    if (type === 'image/svg+xml') return answer({ verdict: 'refused', reason: 'SVG can carry script: attach a PNG' });
    if (type === 'text/plain') return answer(this.#redact(data.key, bytes));
    if (type === 'application/zip') return answer(zipCheck(bytes));
    if (type === 'application/x-tinyactors-snapshot') return answer({ summary: await this.#inspect(bytes) });
    return answer({});
  };

  #redact(key, bytes) {
    let text = bytes.toString('utf8'), redactions = 0;
    for (const pattern of SECRETS) text = text.replace(pattern, () => { redactions++; return '[REDACTED]'; });
    if (redactions === 0) return {};
    const redacted = `${key}.redacted`;
    this.blobs.put(redacted, Buffer.from(text));
    this.blobs.objects.delete(key);       // the original, with its secrets, is not kept
    return { verdict: 'redacted', redactions, key: redacted };
  }

  async #inspect(bytes) {
    if (!this.command) return 'a tinyactors snapshot (no tinyactors command here to read it)';
    let out;
    try { out = execFileSync(this.command, ['inspect', '-'], { input: bytes, timeout: 10_000 }).toString(); }
    catch { return 'a tinyactors snapshot that `tinyactors inspect` could not read'; }
    return out.split('\n').filter((l) => /^- \*\*(Format|System|Time|Segments|Actors)/.test(l)).join('; ') || out.split('\n')[0];
  }
}

const SECRETS = [
  /authorization:\s*(bearer|basic|token)\s+\S+/gi,           // HTTP headers in a log, first: the whole header
  /\b(ghp|gho|ghu|ghs|github_pat)_[A-Za-z0-9_]{20,}/g,      // GitHub tokens
  /\bsk-ant-[A-Za-z0-9_-]{20,}/g,                            // Anthropic API keys
  /\bAKIA[0-9A-Z]{16}\b/g,                                   // AWS access key ids
  /-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----/g,
];

function sniff(b) {
  const head = b.subarray(0, 16), text = b.subarray(0, 64).toString('latin1');
  if (head[0] === 0x89 && text.startsWith('\x89PNG')) return 'image/png';
  if (head[0] === 0xff && head[1] === 0xd8 && head[2] === 0xff) return 'image/jpeg';
  if (text.startsWith('RIFF') && text.slice(8, 12) === 'WEBP') return 'image/webp';
  if (head[0] === 0x1a && head[1] === 0x45 && head[2] === 0xdf && head[3] === 0xa3) return 'video/webm';
  if (text.slice(4, 8) === 'ftyp') return 'video/mp4';
  if (head[0] === 0x1f && head[1] === 0x8b) return 'application/gzip';
  if (text.startsWith('PK\x03\x04')) return 'application/zip';
  if (text.startsWith('tinyactors-snapshot ')) return 'application/x-tinyactors-snapshot';
  const start = b.subarray(0, 512).toString('utf8').trimStart();
  if (/^<\?xml[^>]*>\s*<svg|^<svg/i.test(start)) return 'image/svg+xml';
  if (/^\{"version":\s*[23]/.test(start)) return 'application/x-asciicast';
  if (!b.includes(0) && Buffer.from(b.toString('utf8')).equals(b)) return 'text/plain';
  return 'application/octet-stream';
}

// A zip's entries from its local headers, read, never extracted: refused
// for a path that leaves the archive, a link, or a compression ratio no
// honest log reaches (a zip bomb).
function zipCheck(b) {
  const entries = [];
  let at = 0, declared = 0;
  while (at + 30 <= b.length && b.readUInt32LE(at) === 0x04034b50) {
    const compressed = b.readUInt32LE(at + 18), size = b.readUInt32LE(at + 22);
    const nameLength = b.readUInt16LE(at + 26), extra = b.readUInt16LE(at + 28);
    const name = b.subarray(at + 30, at + 30 + nameLength).toString('utf8');
    if (name.startsWith('/') || name.split('/').includes('..')) return { verdict: 'refused', reason: `an entry leaves the archive: ${name}` };
    declared += size;
    if (compressed > 0 && size / compressed > 100) return { verdict: 'refused', reason: `${name} expands ${Math.round(size / compressed)} times: a zip bomb?` };
    entries.push({ name, size });
    at += 30 + nameLength + extra + compressed;
  }
  if (declared > 500 * 1024 * 1024) return { verdict: 'refused', reason: 'it expands past 500 MB' };
  return { summary: `${entries.length} entries: ${entries.map((e) => `${e.name} (${e.size} B)`).join(', ')}` };
}
