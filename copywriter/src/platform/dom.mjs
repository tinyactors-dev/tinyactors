// The page adapter's DOM side, shared by the extension's content script and
// the embedded widget (AGENTS.md, "Platform adapters"): a selection as a W3C
// text-quote anchor with a CSS path, finding an anchor's text again,
// previewing a replacement by rewriting Text nodes only (never markup),
// reverting it, outlining the active annotation's text, and pick mode
// (a tap picks an element's text; see pick()).
//
//   const page = createDOMPage({ exclude: (node) => widget.contains(node) });
//   page.selection()  page.locate(anchor)  page.preview({id, anchor, text})  ...
//
// Text is compared with its whitespace collapsed, and a space stands
// between blocks, as a reader sees it: a selection's toString() and the
// page's Text nodes then agree.

const SKIPPED = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'TEXTAREA', 'SELECT', 'OPTION', 'svg', 'math', 'IFRAME', 'OBJECT', 'CANVAS']);
const BLOCKS = new Set([
  'ADDRESS', 'ARTICLE', 'ASIDE', 'BLOCKQUOTE', 'BODY', 'BR', 'CAPTION', 'DD', 'DETAILS', 'DIALOG', 'DIV', 'DL', 'DT',
  'FIELDSET', 'FIGCAPTION', 'FIGURE', 'FOOTER', 'FORM', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'HEADER', 'HR', 'LI',
  'MAIN', 'NAV', 'OL', 'P', 'PRE', 'SECTION', 'SUMMARY', 'TABLE', 'TBODY', 'TD', 'TFOOT', 'TH', 'THEAD', 'TR', 'UL',
  'BUTTON', 'LABEL', 'IMG',
]);
// The elements one annotation stays within: a sentence, a heading, a
// paragraph, a list item, a cell, a quote, a caption, a button's or a
// link's label. A selection across two of them, or longer than MAX_TEXT,
// does not fit (it would rewrite a section, not copy).
const UNITS = 'p, li, h1, h2, h3, h4, h5, h6, td, th, blockquote, figcaption, caption, dd, dt, button, a, label, summary';
export const MAX_TEXT = 600;
const CONTEXT = 32;              // characters of prefix and suffix an anchor keeps
const MAX_CONTEXT_TEXT = 20000;  // what context() gives the model
const HIGHLIGHT = 'tinyactors-copywriter';
// What pick mode picks: the nearest of these whose text fits, or any
// element with text of its own that fits.
const PICKABLE = 'p, li, h1, h2, h3, h4, h5, h6, blockquote, figcaption, caption, td, th, dt, dd, button, a, label, summary';
const PINE = '#1F5E4A';
const OVERLAY_Z = 2147482990;    // above the page, below the widget's launcher and sheet
const PREVIEW_HIGHLIGHT = 'tinyactors-copywriter-preview';

const isSpace = (c) => c === ' ' || c === '\n' || c === '\t' || c === '\r' || c === '\f' || c === ' ';

/** Text with its whitespace collapsed to single spaces, trimmed. */
export function normalize(text) {
  return String(text ?? '').replace(/[\s ]+/g, ' ').trim();
}

function blockOf(node) {
  for (let el = node.parentElement; el; el = el.parentElement) {
    if (BLOCKS.has(el.tagName)) return el;
  }
  return null;
}

/**
 * The page's text as a reader sees it: `text` (whitespace collapsed, a
 * space between blocks) and, for each character, the Text node and the
 * offset it came from (`node` -1 for a space between blocks).
 */
export function indexText(root, exclude = () => false) {
  const doc = root.ownerDocument || root;
  const nodes = [];
  const starts = [];   // per node: its offsets' positions in text (Int32Array, length+1)
  const chars = [];
  const at = [];       // per char: [node index, offset]
  let lastBlock;
  const walker = doc.createTreeWalker(root, 0x1 | 0x4, {   // elements and text
    acceptNode(node) {
      if (node.nodeType === 1) {
        if (SKIPPED.has(node.tagName) || exclude(node)) return 2;  // reject the subtree
        return 3;  // skip the element, visit its children
      }
      return 1;
    },
  });
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const block = blockOf(node);
    const data = node.data;
    if (lastBlock !== undefined && block !== lastBlock && chars.length && chars[chars.length - 1] !== ' ') {
      chars.push(' ');
      at.push(-1, 0);
    }
    lastBlock = block;
    const index = nodes.length;
    const map = new Int32Array(data.length + 1);
    for (let i = 0; i < data.length; i++) {
      map[i] = chars.length;
      const c = data[i];
      if (isSpace(c)) {
        if (chars.length === 0 || chars[chars.length - 1] === ' ') continue;
        chars.push(' ');
      } else {
        chars.push(c);
      }
      at.push(index, i);
    }
    map[data.length] = chars.length;
    nodes.push(node);
    starts.push(map);
  }
  return { text: chars.join(''), nodes, starts, at };
}

// A text-node boundary for a range boundary that may sit in an element.
function textBoundary(container, offset, { end }) {
  if (container.nodeType === 3) return [container, offset];
  const doc = container.ownerDocument || container;
  const walker = doc.createTreeWalker(container, 0x4);
  if (!end) {
    const child = container.childNodes[offset];
    if (child) {
      if (child.nodeType === 3) return [child, 0];
      walker.currentNode = child;
      const first = child.nodeType === 1 ? walker.nextNode() : null;
      if (first && child.contains(first)) return [first, 0];
    }
    return [container, offset];
  }
  const child = container.childNodes[offset - 1];
  if (child) {
    if (child.nodeType === 3) return [child, child.data.length];
    let last = null;
    walker.currentNode = child;
    for (let n = walker.nextNode(); n && child.contains(n); n = walker.nextNode()) last = n;
    if (last) return [last, last.data.length];
  }
  return [container, offset];
}

// The position of a text boundary in the index, or -1.
function positionOf(index, node, offset) {
  const i = index.nodes.indexOf(node);
  if (i < 0) return -1;
  return index.starts[i][Math.min(offset, index.starts[i].length - 1)];
}

const elementOf = (node) => (node.nodeType === 1 ? node : node.parentElement);
const unitOf = (node) => elementOf(node)?.closest(UNITS) ?? null;

/**
 * Whether a range stays within one unit (UNITS): its common ancestor is
 * in one, or its two ends are in none and in the same block.
 */
export function withinOneUnit(range) {
  if (unitOf(range.commonAncestorContainer)) return true;
  const [sn] = textBoundary(range.startContainer, range.startOffset, { end: false });
  const [en] = textBoundary(range.endContainer, range.endOffset, { end: true });
  return !unitOf(sn) && !unitOf(en) && blockOf(sn) === blockOf(en);
}

/** A CSS path to `el`: its id when unique, else tag:nth-of-type steps from the nearest one. */
export function cssPath(el) {
  const doc = el.ownerDocument;
  const steps = [];
  for (let node = el; node && node.nodeType === 1; node = node.parentElement) {
    if (node === doc.body || node === doc.documentElement) { steps.unshift(node.tagName.toLowerCase()); break; }
    if (node.id && /^[A-Za-z][\w-]*$/.test(node.id) && doc.querySelectorAll(`#${node.id}`).length === 1) {
      steps.unshift(`#${node.id}`);
      break;
    }
    const tag = node.tagName.toLowerCase();
    let nth = 1;
    for (let s = node.previousElementSibling; s; s = s.previousElementSibling) if (s.tagName === node.tagName) nth++;
    let more = false;
    for (let s = node.nextElementSibling; s && !more; s = s.nextElementSibling) if (s.tagName === node.tagName) more = true;
    steps.unshift(nth > 1 || more ? `${tag}:nth-of-type(${nth})` : tag);
  }
  return steps.join(' > ');
}

/** The anchor of a range: its text quote (exact, prefix, suffix) and the CSS path of its nearest element. */
export function anchorOf(range, { root, exclude } = {}) {
  const doc = range.startContainer.ownerDocument || range.startContainer;
  const index = indexText(root || doc.body, exclude);
  const [sn, so] = textBoundary(range.startContainer, range.startOffset, { end: false });
  const [en, eo] = textBoundary(range.endContainer, range.endOffset, { end: true });
  let start = sn.nodeType === 3 ? positionOf(index, sn, so) : -1;
  let end = en.nodeType === 3 ? positionOf(index, en, eo) : -1;
  if (start < 0 || end < 0 || end <= start) {
    // Fall back to the selected text itself, found in the page.
    const exact = normalize(range.toString());
    start = exact ? index.text.indexOf(exact) : -1;
    if (start < 0) return null;
    end = start + exact.length;
  }
  while (start < end && index.text[start] === ' ') start++;
  while (end > start && index.text[end - 1] === ' ') end--;
  if (end <= start) return null;
  let element = range.commonAncestorContainer;
  if (element.nodeType !== 1) element = element.parentElement;
  return {
    quote: {
      exact: index.text.slice(start, end),
      prefix: index.text.slice(Math.max(0, start - CONTEXT), start),
      suffix: index.text.slice(end, end + CONTEXT),
    },
    selector: element ? cssPath(element) : '',
  };
}

// How well the text around position `at` agrees with the anchor's context.
function score(text, at, length, quote) {
  let s = 0;
  const prefix = normalize(quote.prefix ?? '');
  const suffix = normalize(quote.suffix ?? '');
  const before = text.slice(Math.max(0, at - prefix.length - 1), at).trimEnd();
  const after = text.slice(at + length, at + length + suffix.length + 1).trimStart();
  for (let i = 1; i <= prefix.length && before[before.length - i] === prefix[prefix.length - i]; i++) s++;
  for (let i = 0; i < suffix.length && after[i] === suffix[i]; i++) s++;
  return s;
}

/** A Range over the anchor's text in the page, or null when it is not there. */
export function findRange(anchor, { root, exclude } = {}) {
  const exact = normalize(anchor?.quote?.exact);
  if (!exact) return null;
  const doc = (root || document).ownerDocument || document;
  const index = indexText(root || doc.body, exclude);
  let scope = null;
  try { scope = anchor.selector ? doc.querySelector(anchor.selector) : null; } catch { /* not a selector */ }
  let best = -1;
  let bestScore = -1;
  for (let at = index.text.indexOf(exact); at >= 0; at = index.text.indexOf(exact, at + 1)) {
    let s = score(index.text, at, exact.length, anchor.quote) * 2;
    if (scope && index.at[at * 2] >= 0 && scope.contains(index.nodes[index.at[at * 2]])) s += 1;
    if (s > bestScore) { best = at; bestScore = s; }
  }
  if (best < 0) return null;
  const last = best + exact.length - 1;
  const range = doc.createRange();
  range.setStart(index.nodes[index.at[best * 2]], index.at[best * 2 + 1]);
  range.setEnd(index.nodes[index.at[last * 2]], index.at[last * 2 + 1] + 1);
  return range;
}

// The Text nodes a range covers, each with the part of its data inside it.
function textParts(range) {
  const parts = [];
  const doc = range.startContainer.ownerDocument;
  const root = range.commonAncestorContainer;
  if (root.nodeType === 3) return [{ node: root, from: range.startOffset, to: range.endOffset }];
  const walker = doc.createTreeWalker(root, 0x4);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    if (!range.intersectsNode(node)) continue;
    const from = node === range.startContainer ? range.startOffset : 0;
    const to = node === range.endContainer ? range.endOffset : node.data.length;
    if (to > from) parts.push({ node, from, to });
  }
  return parts;
}

const ownText = (el) => [...el.childNodes].some((n) => n.nodeType === 3 && n.data.trim());

/**
 * The text block pick mode picks for a tap on `target`: the nearest
 * PICKABLE element, or element with text of its own, whose text is not
 * empty and at most MAX_TEXT; null when none (the body, an image alone).
 */
export function blockAt(target, { exclude = () => false } = {}) {
  for (let el = elementOf(target); el && el.tagName !== 'BODY' && el.tagName !== 'HTML'; el = el.parentElement) {
    if (exclude(el) || SKIPPED.has(el.tagName)) return null;
    if (!(el.matches(PICKABLE) || ownText(el))) continue;
    const text = normalize(el.textContent);
    if (text && text.length <= MAX_TEXT) return el;
  }
  return null;
}

/** The block a second tap on `el` picks: the nearest larger block around it that still fits, or null. */
export function parentBlock(el, options) {
  const text = normalize(el.textContent);
  for (let up = blockAt(el.parentElement, options); up; up = blockAt(up.parentElement, options)) {
    if (normalize(up.textContent) !== text) return up;
  }
  return null;
}

/**
 * The adapter's DOM methods over `doc`. `exclude(node)` names what is not
 * the page (the widget). `keepSelection(event)` says whether a selection
 * that went empty keeps the last one (the widget: a tap on the sheet
 * clears the page's selection on a phone). `band()`, when given, is the
 * part of the viewport the reader sees, `{top?, bottom?, left?, right?}`
 * in viewport pixels (the widget: above its sheet, left of its drawer;
 * calling it may lower the sheet): highlight() and a preview with
 * `reveal` scroll their text to its middle, below a sticky header. Without
 * it, they scroll the text to the viewport's middle.
 */
export function createDOMPage({ doc = document, exclude = () => false, keepSelection = () => false, band = null } = {}) {
  const win = doc.defaultView;
  const previews = new Map(); // id → {parts: [{node, before, after}], range, exact}
  let last = null;            // the last non-empty selection, {text, anchor}
  const listeners = new Set();
  let timer = 0;
  let reported = null;

  const options = () => ({ root: doc.body, exclude });
  let picked = null;          // in pick mode: {element, value}

  function current() {
    const selection = doc.getSelection();
    if (!selection || selection.rangeCount === 0 || selection.isCollapsed) return null;
    const range = selection.getRangeAt(0);
    const node = range.commonAncestorContainer;
    if (exclude(node.nodeType === 1 ? node : node.parentElement)) return null;
    const text = normalize(selection.toString());
    if (!text) return null;
    const anchor = anchorOf(range, options());
    if (!anchor) return null;
    const fits = anchor.quote.exact.length <= MAX_TEXT && withinOneUnit(range);
    return { text: anchor.quote.exact, anchor, fits };
  }

  function onSelectionChange() {
    clearTimeout(timer);
    timer = win.setTimeout(() => {
      if (picking) return;  // a pick is the selection until pick mode ends
      const now = current();
      if (now) last = now;
      else if (keepSelection()) return;
      else last = null;
      const value = now || null;
      if ((value && value.text) === (reported && reported.text) && (value && value.fits) === (reported && reported.fits)) return;
      reported = value;
      for (const fn of listeners) fn(value);
    }, 120);
  }
  doc.addEventListener('selectionchange', onSelectionChange);

  // One stylesheet for the highlights, the page's own (adoptedStyleSheets
  // leaves the page's markup alone).
  let styled = false;
  function style() {
    if (styled) return;
    styled = true;
    try {
      const sheet = new win.CSSStyleSheet();
      sheet.replaceSync(`::highlight(${HIGHLIGHT}) { background-color: rgba(31, 94, 74, .22); color: inherit; }
::highlight(${PREVIEW_HIGHLIGHT}) { background-color: rgba(216, 166, 78, .28); text-decoration: underline dotted rgba(31, 94, 74, .8); }`);
      doc.adoptedStyleSheets = [...doc.adoptedStyleSheets, sheet];
    } catch { /* no constructable stylesheets: highlights stay unstyled */ }
  }
  const highlights = () => (win.CSS && win.CSS.highlights && win.Highlight ? win.CSS.highlights : null);

  function showPreviews() {
    const registry = highlights();
    if (!registry) return;
    style();
    const ranges = [...previews.values()].map((p) => p.range).filter((r) => r && !r.collapsed);
    if (ranges.length) registry.set(PREVIEW_HIGHLIGHT, new win.Highlight(...ranges));
    else registry.delete(PREVIEW_HIGHLIGHT);
  }

  function info() {
    const url = new URL(doc.location.href);
    url.hash = '';
    return { url: url.href, title: doc.title || '', lang: doc.documentElement.lang || 'en' };
  }

  function context() {
    const headings = [...doc.querySelectorAll('h1, h2, h3')]
      .filter((h) => !exclude(h) && (h.checkVisibility ? h.checkVisibility() : true))
      .map((h) => normalize(h.textContent)).filter(Boolean).slice(0, 60);
    const index = indexText(doc.body, (el) => exclude(el) || (el.checkVisibility ? !el.checkVisibility() : false));
    return { headings, text: index.text.slice(0, MAX_CONTEXT_TEXT) };
  }

  // The height a sticky or fixed header covers at the top of the viewport.
  function headerBottom() {
    let el = doc.elementFromPoint?.(win.innerWidth / 2, 1);
    for (; el && el !== doc.body && el !== doc.documentElement; el = el.parentElement) {
      if (exclude(el)) return 0;
      const position = win.getComputedStyle(el).position;
      if (position === 'sticky' || position === 'fixed') return Math.max(0, el.getBoundingClientRect().bottom);
    }
    return 0;
  }

  // Scrolls `range` into the middle of what the reader sees, unless it is
  // in view already.
  function reveal(range) {
    const behavior = win.matchMedia?.('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth';
    const area = band ? band() || {} : {};
    const top = Math.max(area.top ?? 0, headerBottom());
    const bottom = area.bottom ?? win.innerHeight;
    const left = area.left ?? 0;
    const right = area.right ?? win.innerWidth;
    const rect = range.getBoundingClientRect();
    const inside = rect.top >= top && rect.bottom <= bottom && rect.left >= left && rect.right <= right;
    if (inside) return;
    if (!band) {
      elementOf(range.startContainer)?.scrollIntoView({ block: 'center', behavior });
      return;
    }
    const dy = rect.height > bottom - top - 16
      ? rect.top - top - 8
      : (rect.top + rect.bottom) / 2 - (top + bottom) / 2;
    const dx = rect.right > right ? Math.min(rect.left - left - 8, rect.right - right + 8) : (rect.left < left ? rect.left - left - 8 : 0);
    win.scrollBy({ top: rect.top >= top && rect.bottom <= bottom ? 0 : dy, left: dx, behavior });
  }

  function preview({ id, anchor, text, reveal: show = false }) {
    if (previews.has(id)) revert({ id });
    const range = findRange(anchor, options());
    if (!range || !withinOneUnit(range)) return false;
    const parts = textParts(range);
    if (!parts.length) return false;
    const replacement = String(text ?? '');
    const done = [];
    parts.forEach(({ node, from, to }, i) => {
      const before = node.data;
      const after = before.slice(0, from) + (i === 0 ? replacement : '') + before.slice(to);
      node.data = after;
      done.push({ node, before, after });
    });
    const first = parts[0];
    const shown = doc.createRange();
    shown.setStart(first.node, first.from);
    shown.setEnd(first.node, first.from + replacement.length);
    previews.set(id, { parts: done, range: shown, exact: normalize(anchor?.quote?.exact) });
    showPreviews();
    if (show) reveal(shown);
    return true;
  }

  function revert({ id }) {
    const p = previews.get(id);
    if (!p) return false;
    previews.delete(id);
    let ok = true;
    for (const { node, before, after } of p.parts) {
      if (node.isConnected && node.data === after) node.data = before;
      else ok = false;
    }
    showPreviews();
    return ok;
  }

  function highlight(anchor) {
    const registry = highlights();
    if (!anchor) {
      registry?.delete(HIGHLIGHT);
      return true;
    }
    const range = findRange(anchor, options());
    if (!range) {
      // previewed: the page shows the replacement where the text was
      const exact = normalize(anchor?.quote?.exact);
      const shown = [...previews.values()].find((p) => p.exact === exact && p.range && !p.range.collapsed);
      registry?.delete(HIGHLIGHT);
      if (!shown) return false;
      reveal(shown.range);
      return true;
    }
    if (registry) {
      style();
      registry.set(HIGHLIGHT, new win.Highlight(range));
    }
    reveal(range);
    return true;
  }

  // ---- pick mode ---------------------------------------------------------------
  //
  // While on: the page's text cannot be selected and shows no callout (a
  // stylesheet of our own, adopted while picking), a click or tap on the
  // page is caught in the capture phase (no link opens, no button
  // presses), a mouse's hover outlines what it would pick (dashed), and a
  // tap picks a block (solid), which is the selection. Both outlines are
  // overlays over the element, never its own styles.
  let picking = false;
  const pickEnds = new Set();
  let pickSheet = null;
  const overlay = { hover: null, picked: null };
  let hovered = null;
  let framed = 0;

  function box(kind) {
    if (overlay[kind]) return overlay[kind];
    const el = doc.createElement('div');
    el.setAttribute('aria-hidden', 'true');
    el.dataset.copywriterOutline = kind;
    el.style.cssText = `position:fixed;z-index:${OVERLAY_Z};pointer-events:none;box-sizing:border-box;border-radius:3px;display:none;`
      + `outline:2px ${kind === 'hover' ? 'dashed' : 'solid'} ${PINE};outline-offset:3px;`
      + (kind === 'picked' ? 'background:rgba(31,94,74,.06);transition:top .14s,left .14s,width .14s,height .14s;' : '');
    doc.documentElement.append(el);
    overlay[kind] = el;
    return el;
  }

  function place() {
    framed = 0;
    for (const [kind, el] of [['hover', hovered], ['picked', picked?.element]]) {
      const node = overlay[kind];
      if (!node) continue;
      if (!el || !el.isConnected || (kind === 'hover' && el === picked?.element)) { node.style.display = 'none'; continue; }
      const r = el.getBoundingClientRect();
      Object.assign(node.style, { display: 'block', top: `${r.top}px`, left: `${r.left}px`, width: `${r.width}px`, height: `${r.height}px` });
    }
  }
  const schedule = () => { if (!framed) framed = win.requestAnimationFrame(place); };

  const outside = (event) => event.composedPath().some((n) => n.nodeType === 1 && exclude(n));

  function pickElement(el) {
    const range = doc.createRange();
    range.selectNodeContents(el);
    const anchor = anchorOf(range, options());
    if (!anchor) return;
    const value = { text: anchor.quote.exact, anchor, fits: anchor.quote.exact.length <= MAX_TEXT };
    picked = { element: el, value };
    box('picked');
    place();
    reported = value;
    for (const fn of listeners) fn(value);
  }

  // A click or tap on the page: it picks, and nothing else happens.
  function onClick(event) {
    if (outside(event)) return;
    event.preventDefault();
    event.stopPropagation();
    event.stopImmediatePropagation();
    if (event.type !== 'click') return;
    const at = blockAt(event.target, { exclude });
    if (!at) return;
    const el = picked && (picked.element === at || picked.element.contains(at)) ? parentBlock(picked.element, { exclude }) || picked.element : at;
    hovered = null;
    pickElement(el);
  }
  function onMove(event) {
    if (event.pointerType !== 'mouse' || outside(event)) { if (hovered) { hovered = null; schedule(); } return; }
    const at = blockAt(event.target, { exclude });
    if (at !== hovered) { hovered = at; box('hover'); schedule(); }
  }
  function onKey(event) {
    if (event.key !== 'Escape' || outside(event)) return;
    event.preventDefault();
    pick(false);
    for (const fn of pickEnds) fn();
  }
  const CAUGHT = ['click', 'auxclick', 'dblclick', 'mousedown', 'mouseup', 'contextmenu', 'submit'];

  /** Pick mode on or off; → true (this page can pick). */
  function pick(on) {
    if (Boolean(on) === picking) return true;
    picking = Boolean(on);
    if (picking) {
      try {
        pickSheet ??= new win.CSSStyleSheet();
        pickSheet.replaceSync('html, body, body * { -webkit-user-select: none !important; user-select: none !important; -webkit-touch-callout: none !important; } body * { cursor: pointer; }');
        doc.adoptedStyleSheets = [...doc.adoptedStyleSheets, pickSheet];
      } catch { /* no constructable stylesheets: text stays selectable */ }
      doc.getSelection()?.removeAllRanges();
      for (const type of CAUGHT) win.addEventListener(type, onClick, true);
      win.addEventListener('pointermove', onMove, true);
      win.addEventListener('keydown', onKey, true);
      win.addEventListener('scroll', schedule, { capture: true, passive: true });
      win.addEventListener('resize', schedule);
    } else {
      if (pickSheet) doc.adoptedStyleSheets = doc.adoptedStyleSheets.filter((x) => x !== pickSheet);
      for (const type of CAUGHT) win.removeEventListener(type, onClick, true);
      win.removeEventListener('pointermove', onMove, true);
      win.removeEventListener('keydown', onKey, true);
      win.removeEventListener('scroll', schedule, { capture: true });
      win.removeEventListener('resize', schedule);
      win.cancelAnimationFrame(framed);
      framed = 0;
      for (const kind of ['hover', 'picked']) { overlay[kind]?.remove(); overlay[kind] = null; }
      hovered = null;
      picked = null;
    }
    return true;
  }

  /**
   * What the page says it is for (website/README.md, "Audiences and page
   * kinds: the convention"): its kind, the audience files it links (absolute
   * URLs, in order: the first is its main audience), the needs it answers as
   * a whole, and each section that names needs or a kind of its own, by its
   * heading's text (or its CSS path when it has none).
   */
  function purpose() {
    const head = doc.head || doc.documentElement;
    const words = (value) => String(value || '').split(/\s+/).filter(Boolean);
    const meta = (name) => head.querySelector(`meta[name="${name}"]`)?.getAttribute('content') || '';
    const kind = meta('page-kind').trim() || null;
    const audiences = [];
    for (const link of head.querySelectorAll('link[rel~="audience"][href]')) {
      try {
        const url = new URL(link.getAttribute('href'), doc.baseURI).href;
        if (!audiences.includes(url)) audiences.push(url);
      } catch { /* not a URL */ }
    }
    const sections = [];
    for (const el of doc.querySelectorAll('[data-answers], [data-kind]')) {
      if (exclude(el)) continue;
      const heading = el.querySelector('h1, h2, h3, h4, h5, h6');
      const selector = cssPath(el);
      sections.push({
        anchor: heading ? normalize(heading.textContent) : selector, selector,
        ...(el.dataset.kind ? { kind: el.dataset.kind.trim() } : {}),
        answers: words(el.dataset.answers),
      });
    }
    return { kind, audiences, answers: words(meta('page-answers')), sections };
  }

  /** The anchor of the first place `text` is on the page, or null. */
  function anchor(text) {
    const range = findRange({ quote: { exact: text } }, options());
    return range ? anchorOf(range, options()) : null;
  }

  return {
    info,
    context,
    purpose,
    /**
     * The selection now, or the last one kept (keepSelection), for an
     * annotation: it is used, so the same text selected again is reported
     * again, and a kept one is not given twice.
     */
    selection() {
      if (picked) return picked.value;
      const value = current() || (keepSelection() ? last : null);
      last = null;
      reported = null;
      return value;
    },
    onSelection(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    locate: (anchor) => Boolean(findRange(anchor, options())),
    anchor,
    preview,
    revert,
    highlight,
    pick,
    onPickEnd(fn) { pickEnds.add(fn); return () => pickEnds.delete(fn); },
    /** Puts every preview back and stops listening (the script is replaced). */
    dispose() {
      pick(false);
      for (const id of [...previews.keys()]) revert({ id });
      highlights()?.delete(HIGHLIGHT);
      highlights()?.delete(PREVIEW_HIGHLIGHT);
      doc.removeEventListener('selectionchange', onSelectionChange);
      clearTimeout(timer);
      listeners.clear();
    },
  };
}
