// One byte range of a file, as a browser asks for a video it seeks in
// (RFC 9110, section 14): the Worker (worker.mjs) serves /assets/videos/
// through it, since the static assets answer every Range with the whole
// file, and tools/serve.mjs serves every file through it. A single range
// only ("bytes=A-B", "bytes=A-", "bytes=-N"); anything else is the whole.
export function ranged(bytes, header) {
  const size = bytes.byteLength;
  const whole = { status: 200, headers: { 'accept-ranges': 'bytes', 'content-length': String(size) }, body: bytes };
  const match = /^bytes=(\d*)-(\d*)$/.exec((header ?? '').trim());
  if (!match || (match[1] === '' && match[2] === '')) return whole;
  let start, end;
  if (match[1] === '') { // the last N bytes
    start = Math.max(0, size - Number(match[2]));
    end = size - 1;
  } else {
    start = Number(match[1]);
    end = match[2] === '' ? size - 1 : Math.min(Number(match[2]), size - 1);
  }
  if (start >= size || start > end) {
    return { status: 416, headers: { 'accept-ranges': 'bytes', 'content-range': `bytes */${size}` }, body: null };
  }
  return {
    status: 206,
    headers: { 'accept-ranges': 'bytes', 'content-range': `bytes ${start}-${end}/${size}`, 'content-length': String(end - start + 1) },
    body: bytes.slice(start, end + 1),
  };
}
