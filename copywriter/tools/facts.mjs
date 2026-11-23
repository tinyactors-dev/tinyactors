// The evals' check for invented facts: numbers, proper nouns, product
// and package names and URLs in the model's copy that the site's pages
// do not state. A heuristic, reported as a line per item, never a gate:
// a capitalized word at a sentence's start is not a name, and a name the
// site spells otherwise is reported.
//
//   siteText(origin, fetchText)  every page of the site's llms.txt, as its Markdown
//   inventedFacts(copy, known)   → {numbers: [], names: [], urls: []} not in `known`

import { parseLLMS } from '../src/core/site.mjs';

/** The text of every page the site's llms.txt lists, as Markdown, joined; '' without one. */
export async function siteText(origin, fetchText) {
  let pages;
  try { pages = parseLLMS(await fetchText(`${origin}/llms.txt`), origin); } catch { return ''; }
  const texts = await Promise.all(pages.map((p) => fetchText(p.md).catch(() => '')));
  return [pages.map((p) => `${p.title} ${p.description}`).join('\n'), ...texts].join('\n');
}

// Words that start a sentence or are common enough in capitals to say nothing.
const COMMON = new Set(['I', 'A', 'An', 'The', 'It', 'Its', 'This', 'That', 'These', 'Those', 'You', 'Your', 'We', 'Our', 'One', 'No', 'Yes',
  'If', 'When', 'Then', 'Use', 'See', 'Read', 'Run', 'Add', 'Each', 'Every', 'Why', 'How', 'What', 'For', 'With', 'And', 'Or', 'But', 'Not', 'On', 'In', 'At', 'To']);

const URLS = /\bhttps?:\/\/[^\s)"'<>]+/g;
// @scope/name, node:x, a call (actor.ask()) and anything in backticks
const PACKAGES = /(?:@[a-z0-9][\w.-]*\/[\w.-]+|\b[a-z][\w]*:[a-z][\w-]+\b|\b[a-z][\w]*\.[a-z][\w]*\(\)|`[^`\n]{2,60}`)/g;
// Capitalized words, CamelCase and acronyms, with what precedes them (a sentence start is not a name)
const NAMES = /(^|[.!?:]\s+|\s|\()([A-Z][\w]*(?:[.-][A-Za-z0-9]+)*)/g;
const NUMBERS = /\d[\d.,]*/g;

const flat = (t) => String(t || '').replace(/\s+/g, ' ');

/**
 * What `copy` states that `known` (the site's text) does not: `{numbers,
 * names, urls}`, each once. A name is a capitalized word not at a
 * sentence's start, CamelCase, an acronym, a package or module name, or
 * code in backticks; a
 * URL counts when neither it nor its path is in the site's text.
 */
export function inventedFacts(copy, known) {
  const text = flat(copy);
  const site = flat(known);
  const lower = site.toLowerCase();
  const numbers = [...new Set((text.match(NUMBERS) || []).map((n) => n.replace(/[.,]$/, '')))].filter((n) => !site.includes(n));
  const urls = [...new Set(text.match(URLS) || [])].map((u) => u.replace(/[.,;:]$/, '')).filter((u) => {
    let path = u;
    try { path = new URL(u).pathname; } catch { /* as it is */ }
    return !site.includes(u) && !(path.length > 1 && site.includes(path));
  });
  const names = new Set();
  const withoutURLs = text.replace(URLS, (u) => (/[.!?]$/.test(u) ? ' . ' : ' '));
  for (const m of withoutURLs.matchAll(NAMES)) {
    const [, before, word] = m;
    const start = before === '' || /[.!?:]\s+$/.test(before);
    const shaped = /[a-z][A-Z]/.test(word) || /^[A-Z0-9]{2,}$/.test(word) || /[.]/.test(word);
    if (COMMON.has(word) || (start && !shaped) || word.length < 2) continue;
    // a compound (MIT-licensed) counts by its name part
    const name = /^[A-Z0-9]{2,}-[a-z]/.test(word) ? word.split('-')[0] : word;
    if (!site.includes(name)) names.add(name);
  }
  for (const m of withoutURLs.matchAll(PACKAGES)) {
    const name = m[0].replace(/^`|`$/g, '').replace(/[.,;:]+$/, '');
    if (!lower.includes(name.toLowerCase())) names.add(name);
  }
  return { numbers, names: [...names], urls };
}

/** One report line of what `inventedFacts` found, or '' when nothing. */
export function factLine(found) {
  const parts = [
    found.numbers.length && `numbers ${found.numbers.join(', ')}`,
    found.names.length && `names ${found.names.join(', ')}`,
    found.urls.length && `URLs ${found.urls.join(', ')}`,
  ].filter(Boolean);
  return parts.length ? `!! not stated on the site: ${parts.join('; ')}` : '';
}
