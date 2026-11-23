// The work packet: what a coding agent needs to apply the kept changes to
// the site's sources. `packet.xml` (valid against spec/work-packet.xsd,
// which the ZIP carries too), `CHANGES.md` (the same as instructions) and
// `README.md`, in a ZIP written here with no dependency.

import { workPacketXSD } from './charts.mjs';

const encoder = new TextEncoder();

/** SHA-256 of a string's UTF-8, as lowercase hexadecimal. */
export async function sha256(text) {
  const digest = await crypto.subtle.digest('SHA-256', encoder.encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

const escapeText = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/\r/g, '&#13;');
const escapeAttribute = (s) => escapeText(s).replace(/"/g, '&quot;').replace(/\n/g, '&#10;').replace(/\t/g, '&#9;');

/** The suggestion an annotation kept, or undefined. */
function keptText(a) {
  return (a.suggestions || []).find((s) => s.id === a.chosenID);
}

/**
 * What a packet holds, from annotation records: the kept changes, and the
 * notes of open annotations (not kept, not dismissed) without a kept
 * replacement. Pages in the order they first appear.
 */
export function packetItems(annotations) {
  const sorted = [...annotations].sort((a, b) => String(a.createdAt).localeCompare(String(b.createdAt)));
  const changes = sorted.filter((a) => a.state === 'kept' && keptText(a));
  const notes = sorted.filter((a) => a.state !== 'kept' && a.state !== 'dismissed' && (a.note || '').trim());
  const pages = [...new Set([...changes, ...notes].map((a) => a.page))];
  return { pages, changes, notes };
}

/**
 * The packet's three texts: `{xml, changes, readme}`. `pages` maps a URL
 * to `{title, lang}` where known. Throws when there is nothing to export.
 */
export async function buildPacket({ annotations, pages = {}, title = 'Copy review', at = new Date().toISOString() }) {
  const items = packetItems(annotations);
  if (items.changes.length === 0 && items.notes.length === 0) {
    throw new Error('Nothing to export: keep a change or write a note first.');
  }
  const pageID = new Map(items.pages.map((url, i) => [url, `page-${i + 1}`]));
  const all = [...items.changes, ...items.notes];
  const number = new Map(all.map((a, i) => [a, i + 1]));

  const sources = [];
  for (const url of items.pages) {
    const given = pages[url]?.lang || all.find((a) => a.page === url)?.lang;
    const lang = /^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$/.test(given || '') ? given : 'en';
    sources.push(`<page id="${pageID.get(url)}" url="${escapeAttribute(url)}" fingerprint="${await sha256(url)}" locale="${escapeAttribute(lang)}"/>`);
  }
  for (const a of all) {
    const n = number.get(a);
    const selector = a.anchor?.selector || 'body';
    sources.push(`<anchor id="anchor-${n}" pageRef="${pageID.get(a.page)}" selector="${escapeAttribute(selector)}" domPath="${escapeAttribute(selector)}" textSha256="${await sha256(a.original)}" textQuote="${escapeAttribute(a.original)}"/>`);
  }
  for (const a of all) {
    const n = number.get(a);
    const text = (a.note || '').trim() || 'Rewrite this text.';
    sources.push(`<observation id="observation-${n}" authorRef="reviewer" anchorRef="anchor-${n}" source="${escapeAttribute(a.source || 'typed')}" recordedAt="${escapeAttribute(a.createdAt || at)}"><text>${escapeText(text)}</text></observation>`);
  }
  for (const a of items.changes) {
    const n = number.get(a);
    sources.push(`<revision id="revision-${n}" patchId="${escapeAttribute(a.id)}" anchorRef="anchor-${n}" observationRef="observation-${n}" accepted="true" expectedSha256="${await sha256(a.original)}" replacementSha256="${await sha256(keptText(a).text)}"/>`);
  }

  const entries = [];
  for (const a of items.changes) {
    const n = number.get(a);
    const kept = keptText(a);
    entries.push(`<copy-change id="item-${n}" observationRef="observation-${n}" anchorRef="anchor-${n}" revisionRef="revision-${n}" operation="replace_text" accepted="true">`
      + `<expected>${escapeText(a.original)}</expected>`
      + `<replacement>${escapeText(kept.text)}</replacement>`
      + `<rationale>${escapeText(kept.rationale || a.note || '')}</rationale>`
      + `<acceptance>${escapeText(`${a.page} shows the replacement exactly where it showed the expected text, in the same element.`)}</acceptance>`
      + '</copy-change>');
  }
  for (const a of items.notes) {
    const n = number.get(a);
    entries.push(`<design-task id="item-${n}" observationRef="observation-${n}" status="open">`
      + `<summary>${escapeText(a.note.trim())}</summary>`
      + `<instructions>${escapeText(`Revise the text "${a.original}" on ${a.page} to address the note.`)}</instructions>`
      + `<acceptance>${escapeText('The reviewer agrees the note is addressed.')}</acceptance>`
      + '</design-task>');
  }

  const payload = `<payload><sources>${sources.join('')}</sources><items>${entries.join('')}</items></payload>`;
  const digest = await sha256(payload);
  const stamp = at.replace(/[^0-9]/g, '').slice(0, 14);
  const xml = `<?xml version="1.0" encoding="UTF-8"?>\n<work-packet id="packet-${stamp}" version="1" projectId="copywriter" sessionId="review-${stamp}" status="ready" payloadDigest="${digest}">`
    + `<title>${escapeText(title)}</title>${payload}<handoff status="not_dispatched" integration="none"/></work-packet>\n`;

  return { xml, changes: changesMarkdown(items, pages, at), readme: readme(items, at) };
}

// A fenced block whose fence no line of the text closes.
function fence(text) {
  let ticks = '```';
  while (text.includes(ticks)) ticks += '`';
  return `${ticks}text\n${text}\n${ticks}`;
}

function changesMarkdown(items, pages, at) {
  const lines = [
    '# Copy changes',
    '',
    `A reviewer read these pages in the browser and approved each change below (${at}).`,
    'Apply them to the website\'s sources:',
    '',
    '1. For each change, find the source file that renders the original text on that page.',
    '   Search for the original text; the anchor (the element\'s CSS selector, the text',
    '   before and after it) tells which occurrence it is.',
    '2. Replace the original with the replacement, exactly, character for character.',
    '   Keep the markup around it; change only the text.',
    '3. If the original is not in the sources (it is generated, or it changed since),',
    '   do not guess: list it in your report.',
    '4. Open notes have no replacement: revise the text to address the note, or',
    '   report why not.',
    '5. Build the site and check each page shows the replacement.',
    '',
  ];
  for (const url of items.pages) {
    const title = pages[url]?.title;
    lines.push(`## ${url}${title ? ` (${title})` : ''}`, '');
    let n = 0;
    for (const a of items.changes.filter((c) => c.page === url)) {
      const kept = keptText(a);
      n += 1;
      lines.push(`### Change ${n}`, '', 'Original:', '', fence(a.original), '', 'Replacement:', '', fence(kept.text), '');
      if (kept.rationale) lines.push(`Why: ${kept.rationale}`, '');
      if ((a.note || '').trim()) lines.push(`Reviewer's note: ${a.note.trim()}`, '');
      lines.push(...anchorLines(a), '');
    }
    for (const a of items.notes.filter((c) => c.page === url)) {
      n += 1;
      lines.push(`### Open note ${n}`, '', 'Text:', '', fence(a.original), '', `Note: ${a.note.trim()}`, '', ...anchorLines(a), '');
    }
  }
  return lines.join('\n');
}

function anchorLines(a) {
  const quote = a.anchor?.quote || {};
  const lines = ['Where:'];
  if (a.anchor?.selector) lines.push(`- element: \`${a.anchor.selector}\``);
  if (quote.prefix) lines.push(`- text before it: ${JSON.stringify(quote.prefix)}`);
  if (quote.suffix) lines.push(`- text after it: ${JSON.stringify(quote.suffix)}`);
  if (lines.length === 1) lines.push('- the first place the original text appears on the page');
  return lines;
}

function readme(items, at) {
  return [
    '# Work packet',
    '',
    `Exported by the tinyactors copywriter on ${at}: ${items.changes.length} kept change(s) and ${items.notes.length} open note(s) on ${items.pages.length} page(s).`,
    '',
    '- `CHANGES.md`: the changes as instructions for a coding agent. Start here.',
    '- `packet.xml`: the same, as data, valid against `work-packet.xsd`.',
    '  SHA-256 digests of each original and replacement let a tool check it',
    '  replaced the text it meant to.',
    '- `work-packet.xsd`: the schema.',
    '',
  ].join('\n');
}

// ---- ZIP (stored, no compression) ----------------------------------------

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/** CRC-32 (ISO 3309), as ZIP uses it. */
export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/**
 * A ZIP of `files` ([{name, data}], data a string or a Uint8Array), each
 * stored uncompressed with a UTF-8 name, dated `date`.
 */
export function zip(files, date = new Date()) {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
  const day = ((Math.max(date.getFullYear(), 1980) - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const file of files) {
    if (!file.name || file.name.startsWith('/') || file.name.split('/').includes('..')) {
      throw new Error(`Unsafe file name in the ZIP: ${file.name}`);
    }
    const name = encoder.encode(file.name);
    const data = typeof file.data === 'string' ? encoder.encode(file.data) : file.data;
    const crc = crc32(data);
    const local = new Uint8Array(30 + name.length);
    const l = new DataView(local.buffer);
    l.setUint32(0, 0x04034b50, true);
    l.setUint16(4, 20, true);
    l.setUint16(6, 0x0800, true); // UTF-8 names
    l.setUint16(8, 0, true); // stored
    l.setUint16(10, time, true);
    l.setUint16(12, day, true);
    l.setUint32(14, crc, true);
    l.setUint32(18, data.length, true);
    l.setUint32(22, data.length, true);
    l.setUint16(26, name.length, true);
    local.set(name, 30);
    const central = new Uint8Array(46 + name.length);
    const c = new DataView(central.buffer);
    c.setUint32(0, 0x02014b50, true);
    c.setUint16(4, 20, true);
    c.setUint16(6, 20, true);
    c.setUint16(8, 0x0800, true);
    c.setUint16(10, 0, true);
    c.setUint16(12, time, true);
    c.setUint16(14, day, true);
    c.setUint32(16, crc, true);
    c.setUint32(20, data.length, true);
    c.setUint32(24, data.length, true);
    c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    central.set(name, 46);
    locals.push(local, data);
    centrals.push(central);
    offset += local.length + data.length;
  }
  const directory = centrals.reduce((n, c) => n + c.length, 0);
  const end = new Uint8Array(22);
  const e = new DataView(end.buffer);
  e.setUint32(0, 0x06054b50, true);
  e.setUint16(8, files.length, true);
  e.setUint16(10, files.length, true);
  e.setUint32(12, directory, true);
  e.setUint32(16, offset, true);
  const out = new Uint8Array(offset + directory + 22);
  let at = 0;
  for (const part of [...locals, ...centrals, end]) { out.set(part, at); at += part.length; }
  return out;
}

/** Reads a stored ZIP back: `[{name, data}]` (for tests and checks). */
export function unzip(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const files = [];
  let at = 0;
  const decoder = new TextDecoder();
  while (view.getUint32(at, true) === 0x04034b50) {
    const size = view.getUint32(at + 18, true);
    const nameLength = view.getUint16(at + 26, true);
    const extra = view.getUint16(at + 28, true);
    const name = decoder.decode(bytes.subarray(at + 30, at + 30 + nameLength));
    const start = at + 30 + nameLength + extra;
    const data = bytes.slice(start, start + size);
    if (crc32(data) !== view.getUint32(at + 14, true)) throw new Error(`CRC mismatch in ${name}`);
    files.push({ name, data });
    at = start + size;
  }
  return files;
}

/** The whole export: `{files, zip, changes}` for the annotations given. */
export async function exportPacket(options) {
  const { xml, changes, readme: text } = await buildPacket(options);
  const files = [
    { name: 'packet.xml', data: xml },
    { name: 'CHANGES.md', data: changes },
    { name: 'README.md', data: text },
    { name: 'work-packet.xsd', data: workPacketXSD },
  ];
  return { files, zip: zip(files), changes };
}
