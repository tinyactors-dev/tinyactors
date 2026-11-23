// The revision plan of one page ("Revise page"; charts/plan.scxml): the
// model's functions and rules, and `planOf`, which reads its calls into
// items and routes each need to the page where it belongs. The page's
// kind and main audience win on this page: a need that belongs on another
// kind of page is answered here with one short line and a link, and fully
// on the page of its kind (an existing one from the site map, or a new
// page). The core never trusts the model to have done so: `planOf` turns a
// rewrite or an add here of a need of another kind into such a link.

import { PAGE_KINDS } from './site.mjs';

/** What a plan item does. */
export const PLAN_ACTIONS = Object.freeze(['rewrite', 'add', 'cut', 'link', 'move']);

/** The most items one plan has. */
export const MAX_PLAN_ITEMS = 12;

/** The functions of a plan. */
export const PLAN_FUNCTIONS = Object.freeze([
  {
    name: 'plan_item',
    description: 'One change of the revision plan: what to do, for which needs, on which page, and how to check it was done.',
    parameters: {
      type: 'object',
      properties: {
        action: {
          type: 'string', enum: [...PLAN_ACTIONS],
          description: 'rewrite: replace original_text here with text; add: add text here (where: the heading it goes under); cut: remove original_text here; link: one short line here (text) that sends the reader to the page that answers the need (target); move: original_text belongs on another page (target), which gets text.',
        },
        needs: { type: 'array', items: { type: 'string' }, description: 'The ids of the needs it serves (audience.need), exactly as listed.' },
        target: { type: 'string', description: '"here" for this page; else the URL or path of the page from the site map that answers the need; "new" for a page that does not exist yet (where: its title, and call new_page for it).' },
        original_text: { type: 'string', description: 'For rewrite, cut, move, and a link that replaces a sentence: the exact text on this page, copied verbatim, one sentence, heading or list item.' },
        text: { type: 'string', description: 'The copy, plain text, in the voice: the replacement, the text to add, the link\'s line, or what the target gets. Empty for cut.' },
        target_text: { type: 'string', description: 'For link: what the target page should say to answer the need fully, when it does not yet.' },
        where: { type: 'string', description: 'For add: the heading of this page\'s section it goes under, copied exactly. For target "new": the new page\'s title.' },
        reason: { type: 'string', description: 'One sentence for the reviewer: which need, and why here or there.' },
        criteria: { type: 'array', items: { type: 'string' }, description: 'One acceptance criterion per need, in the order of needs, that someone can check by reading the site, e.g. "The home page links to the page that explains what is lost when the host dies" or "/guides/snapshots/ answers tech-lead.host-dies".' },
        conflict: { type: 'string', description: 'Only when this item settles a conflict (between audiences\' needs, or a need and the page\'s kind): what conflicted and how it is placed.' },
      },
      required: ['action', 'needs', 'target', 'text', 'reason', 'criteria'],
    },
  },
  {
    name: 'new_page',
    description: 'Propose a page the site lacks, where needs that belong on no existing page are answered.',
    parameters: {
      type: 'object',
      properties: {
        title: { type: 'string', description: 'Its title.' },
        kind: { type: 'string', enum: [...PAGE_KINDS], description: 'Its kind.' },
        audiences: { type: 'array', items: { type: 'string' }, description: 'The ids of its audiences, the main one first.' },
        outline: { type: 'array', items: { type: 'string' }, description: 'Its sections, one line each, in order.' },
        needs: { type: 'array', items: { type: 'string' }, description: 'The ids of the needs it answers.' },
        reason: { type: 'string', description: 'One sentence: why no existing page should answer them.' },
      },
      required: ['title', 'kind', 'audiences', 'outline'],
    },
  },
]);

/** What a plan asks of the model, after the page, the needs, what was found and the site map. */
export const PLAN_RULES = Object.freeze([
  'Plan the revision of this page: decide where each need of its audiences is answered, then the changes that get it there, with plan_item calls (and new_page for a page the site lacks).',
  'The page\'s kind and its main audience win on this page. A need that fits the kind is answered here (rewrite, add or cut). A need that belongs on another kind of page, or matters on this page to no audience but a minor one, gets one short line and a link here (action link, target the page that answers it), and, when that page does not answer it fully yet, target_text saying what it should. Never squeeze a need into a page of the wrong kind.',
  'Pick targets from the site map: a page of the need\'s kind whose audiences include the need\'s audience, its main audience first. When none exists, target "new", where the new page\'s title, and call new_page once for it.',
  'Use what was found: the checklist items partial or missing, the open findings, the open notes. Keep the kept changes; never undo them. Never propose again what the reviewer decided (listed below).',
  'original_text is copied exactly from the page text above: one sentence, heading or list item.',
  'criteria: one per need, in the order of needs, checkable by reading the site.',
  'Use only facts, numbers and names the page or the site map states; never invent one. When a fact is missing, say so in reason and leave it out of text.',
  `At most ${MAX_PLAN_ITEMS} items, those that matter most first. Answer with function calls only.`,
]);

const clean = (value, max = 600) => (typeof value === 'string' ? value.trim().slice(0, max) : '');
const strings = (value, max = 200) => (Array.isArray(value) ? value : typeof value === 'string' && value ? [value] : [])
  .map((x) => clean(x, max)).filter(Boolean);

/** The path of a URL, for the reviewer and the criteria; a URL that does not parse as it is. */
export function shortPath(url) {
  try { return new URL(url).pathname; } catch { return String(url); }
}

const article = (word) => (/^[aeiou]/.test(word) ? 'an' : 'a');

/**
 * The page of the site map where a need of `kind` and audience `siteID`
 * is answered: a page of that kind whose main audience it is, else one
 * that names it, else any of that kind; never `here`. Null: none.
 */
export function bestPage(pages, { kind, siteID, here }) {
  const others = pages.filter((p) => p.kind === kind && p.url !== here);
  return others.find((p) => p.audiences[0] === siteID) || others.find((p) => p.audiences.includes(siteID)) || others[0] || null;
}

/**
 * The plan in a reply: `{items, newPages}` from its plan_item and new_page
 * calls, each routed and normalized against the page:
 *
 *   url, kind        the page planned and its kind
 *   needs            Map needID → {kind?, siteID, text, audience (its name)}
 *   pages            the site map: [{url, path, title, kind, audiences: [siteID]}]
 *   id()             a new item's id
 *
 * An item's target is 'here', an absolute URL or 'new'; its needs are
 * those known (unknown ids dropped when any are known); its criteria
 * [{needID, text, page}], one per need, made up from the need when the
 * model gave none. A rewrite or an add here of a need whose kind is not
 * the page's keeps the needs that fit, and each misfit need becomes a link
 * here (`routed`, with a `conflict` saying where it went) to the best page
 * of its kind, or to a new page; a new page is proposed for those with
 * none. Calls of other names or shapes are dropped.
 */
export function planOf({ calls }, { url, kind, needs = new Map(), pages = [], id }) {
  const origin = (() => { try { return new URL(url).origin; } catch { return ''; } })();
  const herePath = shortPath(url);
  const known = needs.size > 0;
  const target = (value) => {
    const t = clean(value, 400);
    if (!t || /^(here|this page)$/i.test(t)) return 'here';
    if (/^new$/i.test(t)) return 'new';
    let absolute;
    try { absolute = new URL(t, origin || undefined).href; } catch { return 'here'; }
    return absolute === url || shortPath(absolute) === herePath ? 'here' : absolute;
  };
  const titleOf = (t) => pages.find((p) => p.url === t)?.title || shortPath(t);
  const audienceOf = (ids) => [...new Set(ids.map((n) => needs.get(n)?.audience).filter(Boolean))];
  const criterion = (needID, page) => ({
    needID, page,
    text: page === url ? `${herePath} answers ${needID}` : `${page === 'new' ? 'A new page' : shortPath(page)} answers ${needID}`,
  });

  const newPages = [];
  for (const { name, args: a } of calls) {
    if (name !== 'new_page' || !clean(a.title, 160) || !PAGE_KINDS.includes(a.kind)) continue;
    const ids = strings(a.needs).filter((n) => !known || needs.has(n));
    newPages.push({
      id: id(), title: clean(a.title, 160), kind: a.kind, audiences: strings(a.audiences, 80), outline: strings(a.outline, 300).slice(0, 12),
      needs: ids, reason: clean(a.reason), decision: null,
      criteria: ids.map((n) => ({ needID: n, page: 'new', text: `A new ${a.kind} page "${clean(a.title, 160)}" answers ${n}` })),
    });
  }
  const newPageFor = (needKind, siteID) => {
    let p = newPages.find((x) => x.kind === needKind && (!siteID || !x.audiences.length || x.audiences.includes(siteID)));
    if (!p) {
      const audience = [...needs.values()].find((x) => x.siteID === siteID)?.audience;
      const title = `${needKind[0].toUpperCase()}${needKind.slice(1)}${audience ? ` for ${audience}` : ''}`;
      p = { id: id(), title, kind: needKind, audiences: siteID ? [siteID] : [], outline: [], needs: [], reason: 'No page of the site answers these needs.', decision: null, criteria: [] };
      newPages.push(p);
    }
    return p;
  };

  const items = [];
  for (const { name, args: a } of calls) {
    if (name !== 'plan_item' || !PLAN_ACTIONS.includes(a.action)) continue;
    let action = a.action;
    let to = target(a.target);
    const original = clean(a.original_text);
    const text = clean(a.text, 1200);
    if (action === 'cut' ? !original : !text) continue;
    if (to === 'here' && action === 'move') continue;               // a move goes somewhere
    if (to === 'here' && action === 'link') action = original ? 'rewrite' : 'add'; // a link here is a change here
    if (to !== 'here' && (action === 'rewrite' || action === 'add' || action === 'cut')) action = action === 'cut' ? 'cut' : 'move';
    if (to !== 'here' && action === 'cut') continue;                // a cut is of this page
    let ids = strings(a.needs, 120).filter((n) => !known || needs.has(n));
    const given = strings(a.criteria ?? a.criterion, 400);
    const where = clean(a.where, 200);
    if (to === 'new') {
      const p = (where && newPages.find((x) => x.title.toLowerCase() === where.toLowerCase())) || newPageFor(needs.get(ids[0])?.kind || 'explanation', needs.get(ids[0])?.siteID);
      to = 'new';
      for (const n of ids) if (!p.needs.includes(n)) { p.needs.push(n); p.criteria.push({ needID: n, page: 'new', text: `A new ${p.kind} page "${p.title}" answers ${n}` }); }
    }
    const page = to === 'here' ? url : to;
    // a link's criteria are of two pages: here (the line and its link) and the target (the answer);
    // a criterion that names the target's path is the target's
    const targetPath = to !== 'here' && to !== 'new' ? shortPath(to) : '';
    const ofTarget = (text) => Boolean(targetPath && targetPath !== '/' && text.includes(targetPath));
    const criteriaOf = (list) => {
      if (action !== 'link' || to === 'new') {
        return list.map((n) => {
          const i = ids.indexOf(n);
          return given[i] ? { needID: n, page, text: given[i] } : criterion(n, page);
        });
      }
      const there = given.filter(ofTarget);
      const here = given.filter((c) => !ofTarget(c));
      return list.flatMap((n, i) => [
        here[i] ? { needID: n, page: url, text: here[i] } : { needID: n, page: url, text: `${herePath} links to ${shortPath(to)} for ${n}` },
        there[i] ? { needID: n, page: to, text: there[i] } : criterion(n, to),
      ]);
    };
    const base = {
      action, target: to, ...(original ? { original_text: original } : {}), text: action === 'cut' ? '' : text,
      ...(clean(a.target_text, 1200) ? { target_text: clean(a.target_text, 1200) } : {}),
      ...(where ? { where } : {}), reason: clean(a.reason), ...(clean(a.conflict) ? { conflict: clean(a.conflict) } : {}),
      ...(to !== 'here' && to !== 'new' ? { targetTitle: titleOf(to) } : {}),
    };
    // a change here of a need whose kind is not the page's: that need is linked, not answered here
    const misfit = to === 'here' && kind && (action === 'rewrite' || action === 'add')
      ? ids.filter((n) => needs.get(n)?.kind && needs.get(n).kind !== kind) : [];
    const fit = ids.filter((n) => !misfit.includes(n));
    if (!misfit.length || fit.length) {
      ids = fit;
      items.push({ id: id(), ...base, needs: ids, criteria: criteriaOf(ids), audiences: audienceOf(ids), decision: null });
    }
    // one link per page the misfit needs go to
    const routes = new Map();
    for (const n of misfit) {
      const need = needs.get(n);
      const best = bestPage(pages, { kind: need.kind, siteID: need.siteID, here: url });
      const key = best ? best.url : `new:${need.kind}`;
      if (!routes.has(key)) routes.set(key, { best, kind: need.kind, siteID: need.siteID, needs: [] });
      routes.get(key).needs.push(n);
    }
    for (const r of routes.values()) {
      const dest = r.best ? r.best.url : 'new';
      const fresh = r.best ? null : newPageFor(r.kind, r.siteID);
      if (fresh) for (const n of r.needs) if (!fresh.needs.includes(n)) { fresh.needs.push(n); fresh.criteria.push({ needID: n, page: 'new', text: `A new ${fresh.kind} page "${fresh.title}" answers ${n}` }); }
      const title = r.best ? r.best.title : fresh.title;
      const destPath = r.best ? shortPath(r.best.url) : `the new page "${fresh.title}"`;
      const first = needs.get(r.needs[0]);
      const line = `${String(first.text).replace(/[.?!:;]*$/, '')}? See ${title}.`.replace(/\?\? /, '? ');
      const reason = `${r.needs.join(', ')} belong${r.needs.length === 1 ? 's' : ''} on ${article(r.kind)} ${r.kind} page: linked to ${destPath}.`;
      items.push({
        id: id(), action: 'link', target: dest, ...(r.best ? { targetTitle: title } : { where: fresh.title }),
        text: line, target_text: text, reason, conflict: reason, routed: true,
        needs: r.needs, audiences: audienceOf(r.needs), decision: null,
        criteria: r.needs.flatMap((n) => [
          { needID: n, page: url, text: `${herePath} links to ${destPath} for ${n}` },
          { needID: n, page: dest, text: `${r.best ? shortPath(r.best.url) : `The new page "${fresh.title}"`} answers ${n}` },
        ]),
      });
    }
  }
  return { items: items.slice(0, MAX_PLAN_ITEMS * 2), newPages };
}
