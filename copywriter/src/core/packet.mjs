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

const pathOf = (url) => { try { return new URL(url).pathname; } catch { return String(url); } };

/** The suggestion an annotation kept, or undefined. */
function keptText(a) {
  return (a.suggestions || []).find((s) => s.id === a.chosenID);
}

/**
 * What the reviewer asked of an annotation: the note, else the first
 * message they wrote, else ''.
 */
export function request(a) {
  return (a.note || '').trim() || ((a.messages || []).find((m) => m.role === 'user' && m.text !== '(voice message)')?.text || '').trim()
    || (a.finding ? `${a.audiences?.[0] ? `${a.audiences[0]}: ` : ''}${a.finding.explanation}`.trim() : '');
}

/** How a checklist item's field reads as a request to the writer. */
const GAP_VERBS = Object.freeze({ wants: 'Give them', tasks: 'Give them a next step to', objections: 'Answer' });
const STATUS = Object.freeze({ partial: 'partly answered', missing: 'not answered' });

// The proposals of an open annotation, the user's own last.
const proposed = (a) => (a.suggestions || []).filter((s) => s.text);

/**
 * What a packet holds, from annotation records: the kept changes, and
 * every open annotation (not kept, not dismissed: a note, a request, the
 * page chat's proposals) as an open item. Pages in the order they first
 * appear.
 */
export function packetItems(annotations, gaps = [], plans = []) {
  const sorted = [...annotations].sort((a, b) => String(a.createdAt).localeCompare(String(b.createdAt)));
  const changes = sorted.filter((a) => a.state === 'kept' && keptText(a));
  const notes = sorted.filter((a) => a.state !== 'kept' && a.state !== 'dismissed' && a.state !== 'deleted');
  // a gap: one checklist item of one audience's reading, {page, audience, at, field, text, status, explanation}
  const open = gaps.flatMap((g) => g.items.map((x) => ({ page: g.page, audience: g.audience, at: g.at, ...x })));
  // the accepted items of each page's plan: the criteria they name, by page;
  // the tasks for other pages; the proposed pages
  const criteria = new Map(); // url → [{needID?, text}]
  const add = (url, c) => {
    if (!criteria.has(url)) criteria.set(url, []);
    if (!criteria.get(url).some((x) => x.text === c.text)) criteria.get(url).push(c);
  };
  const tasks = [];
  const newPages = [];
  for (const plan of plans) {
    for (const x of plan.items || []) {
      for (const c of x.criteria || []) if (c.page !== 'new') add(c.page || plan.page, c);
      if (x.target !== 'here' && x.target !== 'new') {
        const what = x.action === 'move'
          ? `Move here from ${plan.page}${x.original_text ? ` the text "${x.original_text}"` : ''} and remove it there: ${x.text}`
          : x.target_text || `Answer ${(x.needs || []).join(', ')} fully.`;
        tasks.push({ from: plan.page, page: x.target, title: x.targetTitle || '', action: x.action, needs: x.needs || [], what, reason: x.reason || '',
          criteria: (x.criteria || []).filter((c) => c.page === x.target) });
      }
    }
    for (const p of plan.newPages || []) newPages.push({ from: plan.page, ...p });
  }
  // a kept change that names a need: the need is answered where it was kept
  for (const a of changes) {
    const need = needOf(a);
    if (need && !a.plan) add(a.page, { needID: need, text: `Read as ${a.audiences?.[0] || 'its audience'}, ${pathOf(a.page)} answers ${need}` });
  }
  const pages = [...new Set([...changes, ...notes, ...open].map((a) => a.page).concat([...criteria.keys()].filter((url) => plans.some((p) => p.page === url))))];
  return { pages, changes, notes, gaps: open, criteria, tasks, newPages };
}

// What a gap asks the writer, with its need's reference when it has one.
const gapRequest = (g) => `${GAP_VERBS[g.field] || 'Address'}: ${g.text} (${STATUS[g.status] || g.status} for ${g.audience})${g.needID ? ` [${g.needID}]` : ''}`;

// What a task says of its page's kind: the kind wins, other needs are routed.
const kindNote = (pages, url) => (pages[url]?.kind ? ` The page is a ${pages[url].kind}: a need that belongs on another kind of page gets a link to where it is answered.` : '');

// The need an annotation is about (a reading's finding, or a gap made an open item).
const needOf = (a) => a.finding?.needID || a.finding?.item?.needID || '';

/** A site audience the reviewer edited: what to change in the site's sources, by field. */
const FIELD_ELEMENTS = Object.freeze({ name: 'name', description: 'description', knows: 'knows', wants: 'want', tasks: 'task', objections: 'objection' });
const audienceTask = (a) => `Update the audience "${a.siteID}" in website/content/site.xml (published as ${a.url}): ${a.differences.map((d) => FIELD_ELEMENTS[d.field] || d.field).join(', ')}.`;

// What an open item asks: the request, else the proposals.
function summary(a) {
  const asked = request(a);
  if (asked) return asked;
  const p = proposed(a);
  return p.length ? `Consider: ${p.map((s) => `"${s.text}"`).join(' or ')}` : 'Review this text.';
}

/**
 * The packet's three texts: `{xml, changes, readme}`. `pages` maps a URL
 * to `{title, lang, audiences?}` where known (audiences: the names of the
 * page's own set, which its section in CHANGES.md says it is for). Throws when there is nothing to export.
 */
export async function buildPacket({ annotations, gaps = [], audiences = [], plans = [], pages = {}, title = 'Copy review', at = new Date().toISOString() }) {
  const items = packetItems(annotations, gaps, plans);
  // the site audiences the reviewer edited: {siteID, url, name, differences: [{field, site, yours}]}
  items.audiences = audiences.filter((a) => a.differences?.length);
  if (items.changes.length === 0 && items.notes.length === 0 && items.gaps.length === 0 && items.audiences.length === 0
    && items.tasks.length === 0 && items.newPages.length === 0 && items.criteria.size === 0) {
    throw new Error('Nothing to export: keep a change or write a note first.');
  }
  // the pages reviewed, then the pages a plan gives tasks to and the pages it was made on
  const known = [...new Set([...items.pages, ...items.tasks.map((t) => t.page), ...items.tasks.map((t) => t.from), ...items.newPages.map((p) => p.from)])];
  const pageID = new Map(known.map((url, i) => [url, `page-${i + 1}`]));
  const all = [...items.changes, ...items.notes, ...items.gaps];
  const number = new Map(all.map((a, i) => [a, i + 1]));

  const sources = [];
  for (const url of known) {
    const given = pages[url]?.lang || all.find((a) => a.page === url)?.lang;
    const lang = /^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$/.test(given || '') ? given : 'en';
    sources.push(`<page id="${pageID.get(url)}" url="${escapeAttribute(url)}" fingerprint="${await sha256(url)}" locale="${escapeAttribute(lang)}"/>`);
  }
  // a page-level item (no anchor: what the page lacks) is anchored to the page's body
  const quoteOf = (a) => (items.gaps.includes(a) ? a.text : a.original);
  for (const [i, a] of items.audiences.entries()) {
    sources.push(`<page id="audience-page-${i + 1}" url="${escapeAttribute(a.url)}" fingerprint="${await sha256(a.url)}" locale="en"/>`);
  }
  for (const a of all) {
    const n = number.get(a);
    const selector = a.anchor?.selector || 'body';
    sources.push(`<anchor id="anchor-${n}" pageRef="${pageID.get(a.page)}" selector="${escapeAttribute(selector)}" domPath="${escapeAttribute(selector)}" textSha256="${await sha256(quoteOf(a))}" textQuote="${escapeAttribute(quoteOf(a))}"/>`);
  }
  for (const [i, t] of items.tasks.entries()) {
    const quote = t.needs.join(' ') || t.what;
    sources.push(`<anchor id="task-anchor-${i + 1}" pageRef="${pageID.get(t.page)}" selector="body" domPath="body" textSha256="${await sha256(quote)}" textQuote="${escapeAttribute(quote)}"/>`);
  }
  for (const [i, p] of items.newPages.entries()) {
    sources.push(`<anchor id="new-page-anchor-${i + 1}" pageRef="${pageID.get(p.from)}" selector="body" domPath="body" textSha256="${await sha256(p.title)}" textQuote="${escapeAttribute(p.title)}"/>`);
  }
  for (const [i, a] of items.audiences.entries()) {
    sources.push(`<anchor id="audience-anchor-${i + 1}" pageRef="audience-page-${i + 1}" selector="audience" domPath="audience" textSha256="${await sha256(a.name)}" textQuote="${escapeAttribute(a.name)}"/>`);
  }
  for (const a of all) {
    const n = number.get(a);
    const gap = items.gaps.includes(a);
    const text = gap ? gapRequest(a) : request(a) || (items.notes.includes(a) ? summary(a) : 'Rewrite this text.');
    const need = gap ? '' : needOf(a);
    sources.push(`<observation id="observation-${n}" authorRef="${gap ? 'audience' : 'reviewer'}" anchorRef="anchor-${n}" source="${escapeAttribute(gap ? 'audience' : a.source || 'typed')}" recordedAt="${escapeAttribute((gap ? a.at : a.createdAt) || at)}"><text>${escapeText(need ? `${text} [${need}]` : text)}</text></observation>`);
  }
  items.tasks.forEach((t, i) => {
    sources.push(`<observation id="task-observation-${i + 1}" authorRef="reviewer" anchorRef="task-anchor-${i + 1}" source="plan" recordedAt="${escapeAttribute(at)}"><text>${escapeText(taskSummary(t))}</text></observation>`);
  });
  items.newPages.forEach((p, i) => {
    sources.push(`<observation id="new-page-observation-${i + 1}" authorRef="reviewer" anchorRef="new-page-anchor-${i + 1}" source="plan" recordedAt="${escapeAttribute(at)}"><text>${escapeText(newPageSummary(p))}</text></observation>`);
  });
  items.audiences.forEach((a, i) => {
    sources.push(`<observation id="audience-observation-${i + 1}" authorRef="reviewer" anchorRef="audience-anchor-${i + 1}" source="audience" recordedAt="${escapeAttribute(at)}"><text>${escapeText(audienceTask(a))}</text></observation>`);
  });
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
      + `<rationale>${escapeText([request(a) && `Asked: ${request(a)}`, kept.rationale, writtenFor(a) && `Written for: ${writtenFor(a)}.`,
        sourcesOf(a).length && `Sources: ${sourcesOf(a).map((x) => `${x.title} <${x.uri}>`).join(', ')}.`].filter(Boolean).join(' ') || '')}</rationale>`
      + `<acceptance>${escapeText([`${a.page} shows the replacement exactly where it showed the expected text, in the same element.`, ...criteriaOf(a)].join(' '))}</acceptance>`
      + '</copy-change>');
  }
  for (const a of items.notes) {
    const n = number.get(a);
    const what = a.anchor ? `Revise the text "${a.original}" on ${a.page} to address it` : `Add to ${a.page}${(a.finding?.section || a.plan?.where) ? ` (under "${a.finding?.section || a.plan.where}")` : ''}: ${a.original}`;
    entries.push(`<design-task id="item-${n}" observationRef="observation-${n}" status="open">`
      + `<summary>${escapeText(summary(a))}</summary>`
      + `<instructions>${escapeText(`${what}${writtenFor(a) ? `, for ${writtenFor(a)}` : ''}${needOf(a) ? ` (need ${needOf(a)})` : ''}${proposed(a).length ? `; proposed: ${proposed(a).map((x) => `"${x.text}"`).join(', ')}` : ''}.${kindNote(pages, a.page)}`)}</instructions>`
      + `<acceptance>${escapeText(criteriaOf(a).length ? criteriaOf(a).join(' ') : 'The reviewer agrees the note is addressed.')}</acceptance>`
      + '</design-task>');
  }
  for (const g of items.gaps) {
    const n = number.get(g);
    entries.push(`<design-task id="item-${n}" observationRef="observation-${n}" status="open">`
      + `<summary>${escapeText(gapRequest(g))}</summary>`
      + `<instructions>${escapeText(`Add to ${g.page} what answers it, where it fits${g.explanation ? ` (${g.explanation})` : ''}, using only facts the project can back.${kindNote(pages, g.page)}`)}</instructions>`
      + `<acceptance>${escapeText(`Read as ${g.audience}, the page answers it.`)}</acceptance>`
      + '</design-task>');
  }

  items.tasks.forEach((t, i) => {
    entries.push(`<design-task id="task-item-${i + 1}" observationRef="task-observation-${i + 1}" status="open">`
      + `<summary>${escapeText(taskSummary(t))}</summary>`
      + `<instructions>${escapeText(`${t.what}${t.reason ? ` (${t.reason})` : ''} Planned while revising ${t.from}.`)}</instructions>`
      + `<acceptance>${escapeText(t.criteria.map((c) => c.text).join(' ') || `${t.page} answers ${t.needs.join(', ')}.`)}</acceptance>`
      + '</design-task>');
  });
  items.newPages.forEach((p, i) => {
    entries.push(`<design-task id="new-page-item-${i + 1}" observationRef="new-page-observation-${i + 1}" status="open">`
      + `<summary>${escapeText(newPageSummary(p))}</summary>`
      + `<instructions>${escapeText(`Write a ${p.kind} page "${p.title}" for ${p.audiences.join(', ') || 'its audiences'}${p.outline.length ? `, in sections: ${p.outline.join('; ')}` : ''}.${p.reason ? ` ${p.reason}` : ''} Proposed while revising ${p.from}.`)}</instructions>`
      + `<acceptance>${escapeText((p.criteria || []).map((c) => c.text).join(' ') || `The site has the page "${p.title}".`)}</acceptance>`
      + '</design-task>');
  });
  items.audiences.forEach((a, i) => {
    entries.push(`<design-task id="audience-item-${i + 1}" observationRef="audience-observation-${i + 1}" status="open">`
      + `<summary>${escapeText(audienceTask(a))}</summary>`
      + `<instructions>${escapeText(a.differences.map((d) => `${FIELD_ELEMENTS[d.field] || d.field}: the site says ${JSON.stringify(d.site)}; the reviewer's copy says ${JSON.stringify(d.yours)}`).join('. '))}. Keep each need's id; a new need gets an id of its own.</instructions>`
      + `<acceptance>${escapeText(`${a.url} says what the reviewer's copy says, and every need id a page names still exists.`)}</acceptance>`
      + '</design-task>');
  });

  const payload = `<payload><sources>${sources.join('')}</sources><items>${entries.join('')}</items></payload>`;
  const digest = await sha256(payload);
  const stamp = at.replace(/[^0-9]/g, '').slice(0, 14);
  const xml = `<?xml version="1.0" encoding="UTF-8"?>\n<work-packet id="packet-${stamp}" version="1" projectId="copywriter" sessionId="review-${stamp}" status="ready" payloadDigest="${digest}">`
    + `<title>${escapeText(title)}</title>${payload}<handoff status="not_dispatched" integration="none"/></work-packet>\n`;

  return { xml, changes: changesMarkdown(items, pages, at), readme: readme(items, at) };
}

// The criteria a plan's annotation carries for its own page, as sentences.
const criteriaOf = (a) => (a.plan?.criteria || []).filter((c) => !c.page || c.page === a.page).map((c) => (/[.!?]$/.test(c.text) ? c.text : `${c.text}.`));
const taskSummary = (t) => `${t.action === 'move' ? 'Move to' : 'Answer on'} ${t.page}: ${t.needs.join(', ') || t.what}`;
const newPageSummary = (p) => `A new ${p.kind} page: "${p.title}"${p.needs?.length ? `, answering ${p.needs.join(', ')}` : ''}`;
const sentence = (text) => (/[.!?]$/.test(text) ? text : `${text}.`);

// What the plan said of an annotation made of one of its items.
const planLines = (a) => (a.plan ? [`Plan: ${a.plan.action}${a.plan.needs?.length ? ` for ${a.plan.needs.join(', ')}` : ''}${a.plan.target && a.plan.target !== 'here' ? `, linking to ${a.plan.target}` : ''}`, ''] : []);

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
    '4. Open items were not approved: revise the text to address the request (a',
    '   proposal may help), or report why not. An item marked "add" is not text on',
    '   the page but something the page lacks: add it where it fits.',
    '5. Gaps by audience: what a reader wants, came to do or objects to that the',
    '   page does not answer. Add what answers each, where it fits, from facts the',
    '   project can back (never invent a number or a claim), or report why not.',
    '6. Audiences to update: the reviewer edited an audience the site publishes;',
    '   change its definition in the site\'s sources to match, keeping need ids.',
    '7. Acceptance criteria: each page\'s list says what must hold once its',
    '   changes are made; check each one by reading the built page, and say in',
    '   your report which hold and which do not.',
    '8. Other pages: tasks for pages other than those reviewed, from the plan of',
    '   the page named in each (a need that belongs there, not here). Proposed',
    '   pages: pages the site lacks; outline each in your report and write it',
    '   only when you are asked to.',
    '9. Build the site and check each page shows the replacement.',
    '',
  ];
  for (const url of items.pages) {
    const title = pages[url]?.title;
    lines.push(`## ${url}${title ? ` (${title})` : ''}`, '');
    const purpose = [pages[url]?.kind && `Kind: ${pages[url].kind}.`, pages[url]?.audiences?.length && `For: ${pages[url].audiences.join(', ')}`].filter(Boolean);
    if (purpose.length) lines.push(purpose.join(' '), '');
    let n = 0;
    for (const a of items.changes.filter((c) => c.page === url)) {
      const kept = keptText(a);
      n += 1;
      lines.push(`### Change ${n}`, '', 'Original:', '', fence(a.original), '', 'Replacement:', '', fence(kept.text), '');
      if (request(a)) lines.push(`Request: ${request(a)}`, '');
      if (kept.rationale) lines.push(`Why: ${kept.rationale}`, '');
      if (writtenFor(a)) lines.push(`Written for: ${writtenFor(a)}`, '');
      lines.push(...planLines(a));
      lines.push(...sourceLines(a));
      lines.push(...anchorLines(a), '');
    }
    for (const a of items.notes.filter((c) => c.page === url)) {
      n += 1;
      lines.push(a.anchor ? `### Open item ${n}` : `### Open item ${n}: add`, '', a.anchor ? 'Text:' : 'Add:', '', fence(a.original), '');
      if (a.finding) lines.push(`Found reading as: ${a.audiences?.[0] || 'an audience'} (${a.finding.kind})`, '');
      if (needOf(a)) lines.push(`Need: ${needOf(a)}`, '');
      if (request(a)) lines.push(`Request: ${request(a)}`, '');
      if (writtenFor(a)) lines.push(`Written for: ${writtenFor(a)}`, '');
      lines.push(...planLines(a));
      lines.push(...sourceLines(a));
      for (const p of proposed(a)) lines.push(`Proposed${p.own ? ' (by the reviewer)' : ''}:`, '', fence(p.text), '', ...(p.rationale && !p.own ? [`Why: ${p.rationale}`, ''] : []));
      lines.push(...anchorLines(a), '');
    }
    const gaps = items.gaps.filter((g) => g.page === url);
    if (gaps.length) {
      lines.push('### Gaps by audience', '');
      for (const audience of [...new Set(gaps.map((g) => g.audience))]) {
        const mine = gaps.filter((g) => g.audience === audience);
        lines.push(`${audience} (read ${mine[0].at || 'earlier'}):`, '');
        for (const g of mine) lines.push(`- ${GAP_VERBS[g.field] || 'Address'}: ${g.text} (${STATUS[g.status] || g.status}${g.needID ? `, ${g.needID}` : ''}${g.kind ? `, belongs on ${/^[aeiou]/.test(g.kind) ? 'an' : 'a'} ${g.kind}` : ''})${g.explanation ? `. ${g.explanation}` : ''}`);
        lines.push('');
      }
    }
    const criteria = items.criteria.get(url) || [];
    if (criteria.length) {
      lines.push('### Acceptance criteria', '', ...criteria.map((c) => `- [ ] ${sentence(c.text)}${c.needID && !c.text.includes(c.needID) ? ` (${c.needID})` : ''}`), '');
    }
  }
  if (items.tasks.length) {
    lines.push('## Other pages', '', 'Needs a revision plan placed on another page: answer each there, in that page\'s kind.', '');
    for (const target of [...new Set(items.tasks.map((t) => t.page))]) {
      const mine = items.tasks.filter((t) => t.page === target);
      lines.push(`### ${target}${mine[0].title ? ` (${mine[0].title})` : ''}`, '');
      for (const t of mine) {
        lines.push(`- ${t.needs.length ? `${t.needs.join(', ')}: ` : ''}${t.what}`, `  From the plan of ${t.from}.${t.reason ? ` ${t.reason}` : ''}`);
        for (const c of t.criteria) lines.push(`  - [ ] ${sentence(c.text)}`);
      }
      lines.push('');
    }
  }
  if (items.newPages.length) {
    lines.push('## Proposed pages', '', 'Pages the site lacks, where needs belong that no page answers.', '');
    for (const p of items.newPages) {
      lines.push(`### ${p.title}`, '', `Kind: ${p.kind}. For: ${p.audiences.join(', ') || '(not named)'}.${p.needs?.length ? ` Answers: ${p.needs.join(', ')}.` : ''}`, '');
      if (p.reason) lines.push(p.reason, '');
      if (p.outline?.length) lines.push('Outline:', '', ...p.outline.map((o, i) => `${i + 1}. ${o}`), '');
      if (p.criteria?.length) lines.push('Acceptance criteria:', '', ...p.criteria.map((c) => `- [ ] ${sentence(c.text)}`), '');
      lines.push(`Proposed while revising ${p.from}.`, '');
    }
  }
  if (items.audiences.length) {
    lines.push('## Audiences to update', '', 'The reviewer edited these audiences; the site\'s files say otherwise.', '');
    for (const a of items.audiences) {
      lines.push(`### ${a.name}`, '', audienceTask(a), '');
      for (const d of a.differences) lines.push(`${FIELD_ELEMENTS[d.field] || d.field}:`, '', 'The site:', '', fence(d.site), '', 'The reviewer\'s:', '', fence(d.yours), '');
    }
  }
  return lines.join('\n');
}

// The web pages an annotation's replies were grounded on, once each.
function sourcesOf(a) {
  const all = [];
  for (const m of a.messages || []) for (const x of m.sources || []) if (!all.some((y) => y.uri === x.uri)) all.push(x);
  return all;
}
const sourceLines = (a) => (sourcesOf(a).length ? ['Sources:', ...sourcesOf(a).map((x, i) => `${i + 1}. [${x.title}](${x.uri})`), ''] : []);

// The audiences an annotation's rewrites were written for, as a list of names.
const writtenFor = (a) => (a.audiences || []).join(', ');

function anchorLines(a) {
  const section = a.finding?.section || a.plan?.where;
  if (!a.anchor) return ['Where:', section ? `- under the heading ${JSON.stringify(section)}` : '- where it fits on the page'];
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
    `Exported by the tinyactors copywriter on ${at}: ${items.changes.length} kept change(s), ${items.notes.length} open item(s) and ${items.gaps.length} gap(s) by audience on ${items.pages.length} page(s)${items.tasks.length ? `, ${items.tasks.length} task(s) for other pages` : ''}${items.newPages.length ? `, ${items.newPages.length} proposed page(s)` : ''}${items.audiences.length ? `, and ${items.audiences.length} audience(s) to update` : ''}.`,
    '',
    '- `CHANGES.md`: the changes as instructions for a coding agent. Start here.',
    '- `packet.xml`: the same, as data, valid against `work-packet.xsd`.',
    '  SHA-256 digests of each original and replacement let a tool check it',
    '  replaced the text it meant to.',
    '- `work-packet.xsd`: the schema.',
    '',
    '## Check the criteria, then the claims',
    '',
    `The plan of a page gives acceptance criteria (${[...items.criteria.values()].reduce((n, c) => n + c.length, 0)} here), each under the page`,
    'it names in CHANGES.md, and the tasks for other pages carry their own. Check each',
    'criterion against the built site and report which hold.',
    '',
    'A need is named `audience.need` (`tech-lead.host-dies`): the audience\'s id and the',
    'need\'s, from the site\'s audience files. The site says which needs each page',
    'answers in its sources, not in its HTML: on www.tinyactors.dev, `website/content/`',
    '(pages in `content/pages/*.xml`, guides in `content/guides/*.xml`). A page\'s',
    '`<purpose kind="..." for="...">` gives its kind and audiences, and may name with',
    '`answers` the needs the whole page answers; a `<section answers="...">` names',
    'the needs its own text answers. When a change makes a section answer a need,',
    'add the need\'s id to that section\'s `answers`; when it no longer does, remove',
    'it. Mark only what the text answers. The convention is website/README.md,',
    '"Purposes and audiences in the content" and "Audiences and page kinds: the',
    'convention".',
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
