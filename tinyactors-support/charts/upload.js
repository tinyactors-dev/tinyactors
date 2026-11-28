// The upload's helpers: the kinds of file the tracker takes, and their limits.

// The types it takes (a function: a top-level var would be a variable of every session).
function types() {
  var MB = 1024 * 1024;
  return {
  'image/png': { kind: 'screenshot', max: 25 * MB },
  'image/jpeg': { kind: 'screenshot', max: 25 * MB },
  'image/webp': { kind: 'screenshot', max: 25 * MB },
  'video/webm': { kind: 'recording', max: 100 * MB },
  'video/mp4': { kind: 'recording', max: 100 * MB },
  'application/x-asciicast': { kind: 'recording', max: 25 * MB },   // a terminal session (asciinema .cast)
  'text/plain': { kind: 'log', max: 25 * MB },
  'application/gzip': { kind: 'log', max: 25 * MB },
  'application/zip': { kind: 'archive', max: 50 * MB },
  'application/x-tinyactors-snapshot': { kind: 'tinyactors', max: 50 * MB }
  };
}

function kindOf(type) { var t = types()[type]; return t ? t.kind : null; }

// Why the declared file is refused before any byte is sent, or null.
function refusal(file) {
  var TYPES = types(), MB = 1024 * 1024;
  if (!TYPES[file.type]) return 'not a type the tracker takes: ' + file.type + ' (it takes ' + Object.keys(TYPES).join(', ') + ')';
  if (!(file.size > 0)) return 'an empty file';
  if (file.size > TYPES[file.type].max) return 'larger than ' + TYPES[file.type].max / MB + ' MB, the limit for a ' + TYPES[file.type].kind;
  if (!/^[0-9a-f]{64}$/.test(String(file.sha256))) return 'sha256 is not 64 hexadecimal digits';
  return null;
}

// A name for the store's key: no directories, nothing a shell or a URL reads.
function safeName(name) { return String(name).split(/[\\/]/).pop().replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, 100) || 'file'; }

// retention: seconds until the file goes, null for a maintainer's (kept for good)
function attachment(uploadID, file, key, scan, uploader, keep) {
  return { upload: uploadID, name: file.name, type: file.type, kind: kindOf(file.type), size: file.size,
           sha256: file.sha256, key: key, by: uploader.login, byID: uploader.githubID,
           retention: keep ? null : guestLimits().retentionSeconds,
           scan: { verdict: scan.verdict, redactions: scan.redactions || 0, summary: scan.summary || null } };
}
