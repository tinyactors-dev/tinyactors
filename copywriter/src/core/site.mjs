// What a page says about itself (website/README.md, "Audiences and page
// kinds: the convention"): its kind, the audiences it links (one published
// file each, in the namespace below) and the needs its sections claim to
// answer. This module reads an audience file into the store's shape and
// keeps a site audience's items, edits and divergence from its file.
//
// An audience file is parsed with the browser's DOMParser where there is
// one (the side panel, the widget), else with the small namespace-aware
// parser below (Node.js, the tests); both give the same record.

/** The namespace of an audience file. */
export const AUDIENCE_NS = 'https://www.tinyactors.dev/ns/audience';

/** The kinds of page, in the convention's order: the maintainer's pitch, then Diátaxis's four. */
export const PAGE_KINDS = Object.freeze(['pitch', 'tutorial', 'how-to', 'reference', 'explanation']);

/** What each kind of page requires of its copy, as the system instruction says it. */
export const KIND_RULES = Object.freeze({
  pitch: 'the reader is deciding whether to adopt it: a claim, a demonstration that runs, proof with its source, a next step',
  tutorial: 'the reader learns by doing: steps that work, in order, each with what they will see',
  'how-to': 'the reader has a goal: the way to it, step by step, nothing they do not need on the way',
  reference: 'the reader looks up facts: complete, exact facts, easy to find, no persuasion',
  explanation: 'the reader wants to understand why: reasons, limits and trade-offs, with the alternatives',
});

/** The need elements of an audience file → the fields of a stored audience. */
const ELEMENT_FIELDS = Object.freeze({ want: 'wants', task: 'tasks', objection: 'objections' });
/** The fields an audience file gives, and so the ones a reviewer's edit can make differ. */
export const SITE_FIELDS = Object.freeze(['name', 'description', 'knows', 'wants', 'tasks', 'objections']);
const LIST_FIELDS = Object.freeze(['wants', 'tasks', 'objections']);

// ---- a small namespace-aware XML parser ----------------------------------

const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };
function unescape(text) {
  return text.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);/g, (whole, name) => {
    if (name[0] === '#') return String.fromCodePoint(name[1] === 'x' ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10));
    if (name in ENTITIES) return ENTITIES[name];
    throw new Error(`Unknown entity ${whole}.`);
  });
}

/**
 * Parses XML into `{ns, local, attributes: {name: value}, children: [element | string]}`
 * with each element's namespace resolved from its prefix. Enough for an
 * audience file: elements, attributes, text, entities, CDATA, comments,
 * processing instructions and a DOCTYPE without an internal subset.
 * Throws on a document that is not well-formed in those terms.
 */
export function parseXML(text) {
  let i = 0;
  const fail = (what) => { throw new Error(`Not well-formed XML at ${i}: ${what}.`); };
  const stack = [];
  let root = null;
  const scope = (el, prefix) => {
    for (let k = stack.length - 1; k >= 0; k--) {
      const ns = stack[k].declared[prefix];
      if (ns !== undefined) return ns;
    }
    if (prefix === 'xml') return 'http://www.w3.org/XML/1998/namespace';
    return prefix ? fail(`undeclared prefix ${prefix}`) : null;
  };
  while (i < text.length) {
    const lt = text.indexOf('<', i);
    const chunk = lt < 0 ? text.slice(i) : text.slice(i, lt);
    if (chunk) {
      if (stack.length) stack.at(-1).children.push(unescape(chunk));
      else if (chunk.trim()) fail('text outside the root');
    }
    if (lt < 0) break;
    i = lt;
    if (text.startsWith('<?', i)) {
      const end = text.indexOf('?>', i);
      if (end < 0) fail('an unclosed processing instruction');
      i = end + 2;
    } else if (text.startsWith('<!--', i)) {
      const end = text.indexOf('-->', i);
      if (end < 0) fail('an unclosed comment');
      i = end + 3;
    } else if (text.startsWith('<![CDATA[', i)) {
      const end = text.indexOf(']]>', i);
      if (end < 0 || !stack.length) fail('a misplaced CDATA section');
      stack.at(-1).children.push(text.slice(i + 9, end));
      i = end + 3;
    } else if (text.startsWith('<!', i)) {
      const end = text.indexOf('>', i);
      if (end < 0 || text.slice(i, end).includes('[')) fail('a DOCTYPE this parser does not read');
      i = end + 1;
    } else if (text.startsWith('</', i)) {
      const end = text.indexOf('>', i);
      const name = text.slice(i + 2, end).trim();
      const open = stack.pop();
      if (!open || open.name !== name) fail(`an unexpected </${name}>`);
      i = end + 1;
    } else {
      const match = /^<([^\s/>]+)((?:\s+[^\s=/>]+\s*=\s*(?:"[^"]*"|'[^']*'))*)\s*(\/?)>/.exec(text.slice(i));
      if (!match) fail('a malformed tag');
      const [whole, name, attributeText, empty] = match;
      const raw = {};
      for (const a of attributeText.matchAll(/([^\s=/>]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g)) {
        if (a[1] in raw) fail(`a repeated attribute ${a[1]}`);
        raw[a[1]] = unescape(a[2] ?? a[3]);
      }
      const declared = {};
      for (const [k, v] of Object.entries(raw)) {
        if (k === 'xmlns') declared[''] = v || null;
        else if (k.startsWith('xmlns:')) declared[k.slice(6)] = v;
      }
      const frame = { name, declared, children: [] };
      stack.push(frame);
      const [prefix, local] = name.includes(':') ? name.split(':', 2) : ['', name];
      const attributes = {};
      for (const [k, v] of Object.entries(raw)) {
        if (k === 'xmlns' || k.startsWith('xmlns:')) continue;
        // an attribute without a prefix is in no namespace; prefixed ones are named by their local part
        if (k.includes(':')) { const [p, l] = k.split(':', 2); scope(frame, p); attributes[`{${scope(frame, p)}}${l}`] = v; } else attributes[k] = v;
      }
      const element = { ns: scope(frame, prefix), local, attributes, children: frame.children };
      if (stack.length > 1) stack.at(-2).children.push(element);
      else if (root) fail('a second root');
      else root = element;
      if (empty) stack.pop();
      i += whole.length;
    }
  }
  if (stack.length || !root) fail('an unclosed element');
  return root;
}

// The same tree from DOMParser's document, so the two paths agree.
function fromDOM(node) {
  const attributes = {};
  for (const a of node.attributes) {
    if (a.name === 'xmlns' || a.prefix === 'xmlns') continue;
    attributes[a.namespaceURI ? `{${a.namespaceURI}}${a.localName}` : a.name] = a.value;
  }
  const children = [];
  for (const c of node.childNodes) {
    if (c.nodeType === 1) children.push(fromDOM(c));
    else if (c.nodeType === 3 || c.nodeType === 4) children.push(c.data);
  }
  return { ns: node.namespaceURI || null, local: node.localName, attributes, children };
}

function parse(text, DOMParser) {
  if (!DOMParser) return parseXML(text);
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.getElementsByTagName('parsererror').length) throw new Error('Not well-formed XML.');
  return fromDOM(doc.documentElement);
}

const textOf = (el) => el.children.map((c) => (typeof c === 'string' ? c : textOf(c))).join('');
const clean = (s) => String(s || '').replace(/\s+/g, ' ').trim();

/**
 * An audience file's text → `{id, name, description, knows, items: [{id,
 * field, text}]}`, its needs in the file's order. Throws when the root is
 * not an audience of AUDIENCE_NS, or a part the convention requires is
 * missing.
 */
export function parseAudience(text, { DOMParser = globalThis.DOMParser } = {}) {
  const root = parse(String(text), DOMParser);
  if (root.ns !== AUDIENCE_NS || root.local !== 'audience') throw new Error(`Not an audience file (its root is {${root.ns}}${root.local}).`);
  const id = clean(root.attributes.id);
  if (!/^[A-Za-z_][\w.-]*$/.test(id)) throw new Error('An audience file needs an id.');
  const record = { id, name: '', description: '', knows: '', items: [] };
  for (const el of root.children) {
    if (typeof el === 'string' || el.ns !== AUDIENCE_NS) continue;
    if (el.local === 'name' || el.local === 'description' || el.local === 'knows') record[el.local] = clean(textOf(el));
    else if (ELEMENT_FIELDS[el.local]) {
      const need = clean(el.attributes.id);
      const words = clean(textOf(el));
      if (need && words) record.items.push({ id: need, field: ELEMENT_FIELDS[el.local], text: words });
    }
  }
  if (!record.name) throw new Error(`The audience ${id} has no name.`);
  return record;
}

/** A parsed file's fields as a stored audience has them: each need one line. */
export function fieldsOf(site) {
  const fields = { name: site.name, description: site.description || '' };
  if (site.knows) fields.knows = site.knows;
  for (const field of LIST_FIELDS) {
    const lines = site.items.filter((x) => x.field === field).map((x) => x.text);
    if (lines.length) fields[field] = lines.join('\n');
  }
  return fields;
}

const linesOf = (text) => String(text || '').split('\n').map(clean).filter(Boolean);
const slug = (text) => clean(text).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 40) || 'need';

/**
 * A site audience's items after the reviewer edited it: one per line of
 * its wants, tasks and objections. A line keeps the id of the file's item
 * with the same text, else of the item at its place in that field (a
 * reworded need is the same need), else gets one made of its words.
 */
export function itemsOf(audience) {
  const known = audience.site?.items || audience.items || [];
  const items = [];
  const used = new Set();
  for (const field of LIST_FIELDS) {
    const before = known.filter((x) => x.field === field);
    const lines = linesOf(audience[field]);
    lines.forEach((text, i) => {
      let id = before.find((x) => x.text === text && !used.has(x.id))?.id;
      if (!id && before[i] && !used.has(before[i].id) && !lines.includes(before[i].text)) id = before[i].id;
      if (!id) { id = slug(text); for (let n = 2; used.has(id) || known.some((x) => x.id === id && x.text !== text); n++) id = `${slug(text)}-${n}`; }
      used.add(id);
      items.push({ id, field, text });
    });
  }
  return items;
}

/**
 * The fields where a site audience differs from its file's copy (`site`):
 * `[{field, site, yours}]`, each as text; none for an audience of another source.
 */
export function differences(audience) {
  if (audience?.source !== 'site' || !audience.site) return [];
  const theirs = fieldsOf(audience.site);
  const out = [];
  for (const field of SITE_FIELDS) {
    const a = linesOf(audience[field]).join('\n');
    const b = linesOf(theirs[field]).join('\n');
    if (a !== b) out.push({ field, site: theirs[field] || '', yours: audience[field] || '' });
  }
  return out;
}

/** A site audience with its items and divergence worked out again after an edit. */
export function edited(audience) {
  if (audience?.source !== 'site') return audience;
  const next = { ...audience, items: itemsOf(audience) };
  next.diverged = differences(next).length > 0;
  return next;
}
