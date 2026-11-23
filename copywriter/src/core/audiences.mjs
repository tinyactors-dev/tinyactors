// Audiences: who reads a page. A site audience (source 'site') comes from
// a file the page links (src/core/site.mjs) and keeps that file's needs as
// items with their ids; any other is the reviewer's or the model's prose.

/** The fields of an audience a reading checks, in order, with their labels. */
export const CHECKLIST_FIELDS = Object.freeze([['wants', 'Wants'], ['tasks', 'Came to'], ['objections', 'Objections']]);

/**
 * An audience's checklist: each item of its wants, tasks and objections,
 * `[{field, text, id?, needID?}]`. A site audience's are its file's needs,
 * in order, each with its id and its reference (`audience.need`). For any
 * other, a field of quoted questions ("…?" "…?") is one item
 * per quote; any other is one item per part between semicolons, and per
 * sentence within a part. Tasks are lists of steps, so a comma parts them
 * too ("Decide yes or no, write the case, check the license": three).
 */
export function checklist(audience) {
  if (audience?.items?.length) {
    return audience.items.map((x) => ({
      field: x.field, text: x.text, ...(x.id ? { id: x.id, ...(audience.siteID ? { needID: `${audience.siteID}.${x.id}` } : {}) } : {}),
    }));
  }
  const items = [];
  for (const [field] of CHECKLIST_FIELDS) {
    const text = String(audience?.[field] || '').trim();
    if (!text) continue;
    const quotes = text.match(/["“][^"”]+["”]/g);
    if (quotes && text.replace(/["“][^"”]+["”]/g, '').replace(/[\s,;.]/g, '') === '') {
      for (const q of quotes) items.push({ field, text: q.trim() });
      continue;
    }
    // semicolons and sentence ends, but not inside a quotation
    const parts = [];
    let current = '';
    let quoted = false;
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (c === '"' || c === '“' || c === '”') quoted = c === '"' ? !quoted : c === '“';
      const sentenceEnd = /[.?!]/.test(c) && /\s/.test(text[i + 1] || '') && /[A-Z"“]/.test(text.slice(i + 1).trimStart()[0] || '');
      if (!quoted && (c === ';' || sentenceEnd || (field === 'tasks' && c === ','))) {
        if (sentenceEnd) current += c;
        parts.push(current);
        current = '';
      } else {
        current += c;
      }
    }
    parts.push(current);
    for (const p of parts) {
      const item = p.trim().replace(/[.;]$/, '').trim();
      if (item) items.push({ field, text: field === 'tasks' ? item : item[0].toUpperCase() + item.slice(1) });
    }
  }
  return items;
}
