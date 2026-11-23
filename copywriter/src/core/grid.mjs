// The site view and verification: what the readings and plans of every
// page say about where each need of the site's audiences is answered
// (`siteGrid`), a reading's history and how a page changed between two
// readings (`summaryOf`, `compare`), and the hash of a page's text that
// tells when it changed (`textHash`).

import { sha256 } from './packet.mjs';
import { shortPath } from './plan.mjs';

/** The most earlier readings a reading keeps. */
export const MAX_HISTORY = 5;

const RANK = Object.freeze({ missing: 0, partial: 1, answered: 2 });
const norm = (text) => String(text || '').replace(/\s+/g, ' ').trim();

/** The hash of a page's text, whitespace collapsed: 16 hex digits. */
export async function textHash(text) {
  return (await sha256(norm(text))).slice(0, 16);
}

/** What a reading's history keeps of it: when, the text's hash, each item's status. */
export function summaryOf(r) {
  return {
    at: r.at || r.started || null, hash: r.hash || null,
    items: (r.items || []).map((x) => ({ field: x.field, text: x.text, status: x.status, ...(x.needID ? { needID: x.needID } : {}) })),
  };
}

/** A new reading's history: the one it replaces, then that one's, newest first. */
export function historyOf(before) {
  if (!before || before.state !== 'done') return [...(before?.history || [])].slice(0, MAX_HISTORY);
  return [summaryOf(before), ...(before.history || [])].slice(0, MAX_HISTORY);
}

const answeredOf = (items) => items.filter((x) => x.status === 'answered').length;
const sameItem = (a, b) => (a.needID || b.needID ? a.needID === b.needID : a.text === b.text);

/**
 * A reading against the one before it (its history's first): `{before:
 * {at, answered, total}, after: {answered, total}, improved, regressed,
 * unchanged}`, each change `{needID?, text, from, to}`; null without one.
 * Improved: a status that rose (missing → partial → answered); regressed:
 * one that fell. Items new in this reading count in neither.
 */
export function compare(r) {
  const last = r?.history?.[0];
  if (!last || r.state !== 'done') return null;
  const improved = [];
  const regressed = [];
  let unchanged = 0;
  for (const x of r.items || []) {
    const was = last.items.find((y) => sameItem(x, y));
    if (!was) continue;
    const change = { ...(x.needID ? { needID: x.needID } : {}), text: x.text, from: was.status, to: x.status };
    if (RANK[x.status] > RANK[was.status]) improved.push(change);
    else if (RANK[x.status] < RANK[was.status]) regressed.push(change);
    else unchanged += 1;
  }
  return {
    before: { at: last.at, answered: answeredOf(last.items), total: last.items.length },
    after: { answered: answeredOf(r.items || []), total: (r.items || []).length },
    improved, regressed, unchanged,
  };
}

const samePage = (a, b) => a === b || shortPath(a) === shortPath(b);

/**
 * The acceptance criteria the accepted plans set for page `url`, from
 * every page's plan: `[{id, text, needID?, page, from: {page, itemID}}]`,
 * once each. `standing(page, annotationID)`: whether an accepted item's
 * annotation still counts (not dismissed or deleted).
 */
export function criteriaFor(url, plans, standing = () => true) {
  const out = [];
  for (const p of plans) {
    for (const x of p.items || []) {
      if (x.decision !== 'accepted' || (x.annotationID && !standing(p.page, x.annotationID))) continue;
      (x.criteria || []).forEach((c, i) => {
        if (!c.page || c.page === 'new' || !samePage(c.page, url)) return;
        if (out.some((y) => y.text === c.text)) return;
        out.push({ id: `${shortPath(p.page)}#${x.id}#${i}`, text: c.text, ...(c.needID ? { needID: c.needID } : {}), page: url, from: { page: p.page, itemID: x.id } });
      });
    }
  }
  return out;
}

/**
 * The site view: every need of the site's audiences against the pages.
 *
 *   map        the site map: [{url, path, title, kind, audiences: [siteID]}]
 *   audiences  the stored audiences (the site's have siteID and items)
 *   readings   every stored reading's record (charts/reading.scxml)
 *   plans      every stored plan's record (charts/plan.scxml)
 *   here       the open page's URL
 *
 * → `{pages: [{url, path, title, kind, audiences: [audienceID], read, at}],
 *     audiences: [{id, name, pages: [url], answered, needs: [{id, field,
 *     text, answered, cells: [{url, status, explanation?}], routes: [{url,
 *     title, from}], newPages: [{title, decision}]}]}]}`.
 * An audience's pages are those the site map gives it (its main pages
 * first) and any other read as it; a cell's status is its latest reading's
 * ('answered' | 'partial' | 'missing'), or null when that page was never
 * read as the audience (unknown). A route is an accepted plan item that
 * sends the need to a page; a new page one not rejected that answers it.
 * A need's `answered`: the pages whose reading answers it.
 */
export function siteGrid({ map = [], audiences = [], readings = [], plans = [], here = '' }) {
  const site = audiences.filter((a) => a.source === 'site' && a.siteID);
  const bySite = new Map(site.map((a) => [a.siteID, a]));
  const pageOf = (url) => map.find((p) => samePage(p.url, url));
  const pages = map.map((p) => {
    const ids = p.audiences.map((s) => bySite.get(s)?.id).filter(Boolean);
    const done = readings.filter((r) => samePage(r.page, p.url) && r.state === 'done');
    return {
      url: p.url, path: p.path, title: p.title, kind: p.kind, audiences: ids,
      read: done.length > 0, at: done.map((r) => r.at).sort().at(-1) || null, ...(samePage(p.url, here) ? { here: true } : {}),
    };
  });
  // the routes and new pages of every plan, by need
  const routes = new Map();
  const fresh = new Map();
  for (const p of plans) {
    for (const x of p.items || []) {
      if (x.decision !== 'accepted' || x.target === 'here') continue;
      for (const n of x.needs || []) {
        const url = x.target === 'new' ? 'new' : x.target;
        if (!routes.has(n)) routes.set(n, []);
        if (!routes.get(n).some((r) => r.url === url)) routes.get(n).push({ url, title: x.target === 'new' ? x.where || 'A new page' : pageOf(x.target)?.title || shortPath(x.target), from: p.page });
      }
    }
    for (const np of p.newPages || []) {
      if (np.decision === 'rejected') continue;
      for (const n of np.needs || []) {
        if (!fresh.has(n)) fresh.set(n, []);
        if (!fresh.get(n).some((y) => y.title === np.title)) fresh.get(n).push({ title: np.title, decision: np.decision || null });
      }
    }
  }
  const out = site.map((a) => {
    const main = map.filter((p) => p.audiences[0] === a.siteID).map((p) => p.url);
    const named = map.filter((p) => p.audiences.includes(a.siteID) && !main.includes(p.url)).map((p) => p.url);
    const read = readings.filter((r) => r.audienceID === a.id && r.state === 'done').map((r) => r.page)
      .filter((u) => ![...main, ...named].some((x) => samePage(x, u)));
    const columns = [...main, ...named, ...new Set(read)];
    const last = (url) => readings.find((r) => r.audienceID === a.id && r.state === 'done' && samePage(r.page, url));
    const needs = (a.items || []).map((need) => {
      const id = `${a.siteID}.${need.id}`;
      const cells = columns.map((url) => {
        const item = last(url)?.items?.find((x) => (x.needID ? x.needID === id : x.text === need.text));
        return { url, status: item?.status ?? null, ...(item?.explanation ? { explanation: item.explanation } : {}) };
      });
      return {
        id, field: need.field, text: need.text, cells, routes: routes.get(id) || [], newPages: fresh.get(id) || [],
        answered: cells.filter((c) => c.status === 'answered').map((c) => c.url),
      };
    });
    return { id: a.id, name: a.name, siteID: a.siteID, pages: columns, needs, answered: needs.filter((n) => n.answered.length).length };
  });
  return { pages, audiences: out };
}
