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
  const others = pages.filter((p) => p.kind === kind && p.url !== here && p.path !== shortPath(here));
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
  const hereName = herePath === '/' ? 'The home page' : herePath;
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
    text: page === url ? `${hereName} answers ${needID}` : `${page === 'new' ? 'A new page' : shortPath(page)} answers ${needID}`,
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
    const fresh = to === 'new'
      ? (where && newPages.find((x) => x.title.toLowerCase() === where.toLowerCase())) || newPageFor(needs.get(ids[0])?.kind || 'explanation', needs.get(ids[0])?.siteID)
      : null;
    const page = to === 'here' ? url : to;
    // a link's criteria are of two pages: here (the line and its link) and the target (the answer).
    // One about a link, or whose subject is this page, is here's; any other the target's.
    const targetPath = to !== 'here' && to !== 'new' ? shortPath(to) : '';
    const isHere = (c) => /\blinks? (to|from)\b/i.test(c) || (herePath === '/' ? /^the home page\b/i.test(c) : c.startsWith(herePath))
      || (targetPath && !c.includes(targetPath) && !/answers/.test(c));
    const order = ids;
    const criteriaOf = (list) => {
      if (action !== 'link') {
        return list.map((n) => {
          const i = order.indexOf(n);
          return given[i] ? { needID: n, page, text: given[i] } : criterion(n, page);
        });
      }
      const here = given.filter(isHere);
      const there = given.filter((c) => !isHere(c));
      return list.flatMap((n, i) => [
        here[i] ? { needID: n, page: url, text: here[i] } : { needID: n, page: url, text: `${hereName} links to ${fresh ? `the new page "${fresh.title}"` : shortPath(to)} for ${n}` },
        there[i] ? { needID: n, page: to, text: there[i] } : criterion(n, to),
      ]);
    };
    const base = {
      action, target: to, ...(original ? { original_text: original } : {}), text: action === 'cut' ? '' : text,
      ...(clean(a.target_text, 1200) ? { target_text: clean(a.target_text, 1200) } : {}),
      ...(where || fresh ? { where: fresh ? fresh.title : where } : {}), reason: clean(a.reason),
      // an item that places a need elsewhere says why: the conflict it settles, else its reason
      ...(clean(a.conflict) || (to !== 'here' && clean(a.reason)) ? { conflict: clean(a.conflict) || clean(a.reason) } : {}),
      ...(to !== 'here' && to !== 'new' ? { targetTitle: titleOf(to) } : {}),
    };
    // a change here of a need whose kind is not the page's: that need is linked, not answered here
    const misfit = to === 'here' && kind && (action === 'rewrite' || action === 'add')
      ? ids.filter((n) => needs.get(n)?.kind && needs.get(n).kind !== kind) : [];
    const fit = ids.filter((n) => !misfit.includes(n));
    if (!misfit.length || fit.length) {
      const criteria = criteriaOf(fit);
      items.push({ id: id(), ...base, needs: fit, criteria, audiences: audienceOf(fit), decision: null });
      // a new page answers the needs sent to it, by the criteria the plan gave for it
      if (fresh) {
        for (const n of fit) {
          const c = criteria.find((x) => x.needID === n && x.page === 'new') || criterion(n, 'new');
          const at = fresh.needs.indexOf(n);
          if (at < 0) { fresh.needs.push(n); fresh.criteria.push({ ...c, page: 'new' }); }
          else fresh.criteria = fresh.criteria.map((x) => (x.needID === n && /^A new \S+ page "/.test(x.text) ? { ...c, page: 'new' } : x));
        }
      }
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
          { needID: n, page: url, text: `${hereName} links to ${destPath} for ${n}` },
          { needID: n, page: dest, text: `${r.best ? shortPath(r.best.url) : `The new page "${fresh.title}"`} answers ${n}` },
        ]),
      });
    }
  }
  return { items: items.slice(0, MAX_PLAN_ITEMS * 2), newPages };
}

// ---- checking acceptance criteria ------------------------------------------

/** The one function of a criteria check: whether the page meets each criterion. */
export const CHECK_FUNCTIONS = Object.freeze([
  {
    name: 'report_criterion',
    description: 'Report whether the page meets one acceptance criterion. Call it once for every criterion.',
    parameters: {
      type: 'object',
      properties: {
        number: { type: 'integer', description: 'The criterion\'s number in the list.' },
        met: { type: 'boolean', description: 'True when the page, as given, meets it.' },
        quote: { type: 'string', description: 'The exact text on the page (or a link of the page, as listed) that shows it is met, or that comes closest; copied verbatim, one sentence. Empty when nothing does.' },
      },
      required: ['number', 'met', 'quote'],
    },
  },
]);

/** What a criteria check asks of the model, after the page and the criteria. */
export const CHECK_RULES = Object.freeze([
  'Check each acceptance criterion against this page alone: its text and its links as listed. A criterion about a link is met only when the page has a link to that page.',
  'Call report_criterion once for every criterion, by its number: met true or false, and the quote that shows it.',
  'Judge strictly: a criterion is met when the page says it, not when the page hints at it. Answer with function calls only.',
]);

/**
 * A criteria check's answer, against the criteria asked and the page's
 * text: `[{...criterion, met: true | false | null, quote?}]`, one per
 * criterion (null: the model said nothing of it; a quote not on the page,
 * whitespace and case aside, is dropped).
 */
export function checksOf({ calls }, criteria, text = '') {
  const flat = String(text).replace(/\s+/g, ' ').toLowerCase();
  const said = new Map();
  for (const { name, args: a } of calls) {
    const n = Number(a?.number);
    if (name !== 'report_criterion' || !Number.isInteger(n) || n < 1 || n > criteria.length || typeof a.met !== 'boolean') continue;
    const quote = clean(a.quote, 400);
    said.set(n - 1, { met: a.met, ...(quote && flat.includes(quote.replace(/\s+/g, ' ').toLowerCase()) ? { quote } : {}) });
  }
  return criteria.map((c, i) => ({ ...c, ...(said.get(i) || { met: null }) }));
}
