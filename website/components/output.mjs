// <ta-output data-format="markdown|json|jsonl|xml">: an output block a reader
// can see as it was printed (Source) or as what it means (Preview): a
// Markdown report rendered, JSON and XML as trees whose branches open and
// close, an SVG drawn. The page renders the source and a hidden switch in
// the block's head (render/site.xsl); this builds the preview and shows the
// switch. The preview is built from the text with DOM nodes, never as
// HTML, so an output cannot inject markup. What the reader picks holds for
// every output of the page, and on the next page (localStorage, when
// allowed). Without this script, or when the text does not parse as its
// format, the block stays source only.

const KEY = 'tinyactors:output-view';
const EVENT = 'ta-output-view';
const OPEN_DEPTH = 2; // JSON and XML branches open up to this depth

const el = (tag, className, ...children) => {
  const node = document.createElement(tag);
  if (className) node.className = className;
  for (const child of children) if (child != null) node.append(child);
  return node;
};

function saved() {
  try { return localStorage.getItem(KEY); } catch { return null; }
}

function save(view) {
  try { localStorage.setItem(KEY, view); } catch { /* private mode: the page keeps it */ }
}

// ---------------------------------------------------------------- Markdown

// Inline Markdown: `code`, **strong**, *emphasis*, [text](link) with an
// http(s) or relative address; everything else is text.
function inline(text) {
  const out = document.createDocumentFragment();
  const pattern = /`([^`]+)`|\*\*([^*]+)\*\*|\*([^*\s][^*]*)\*|\[([^\]]+)\]\(([^)\s]+)\)/g;
  let at = 0;
  for (const match of text.matchAll(pattern)) {
    if (match.index > at) out.append(text.slice(at, match.index));
    if (match[1] !== undefined) out.append(el('code', null, match[1]));
    else if (match[2] !== undefined) out.append(el('strong', null, inline(match[2])));
    else if (match[3] !== undefined) out.append(el('em', null, inline(match[3])));
    else {
      const href = match[5];
      if (/^(https?:\/\/|\/|\.{0,2}\/|#)/.test(href)) {
        const a = el('a', null, inline(match[4]));
        a.href = href;
        out.append(a);
      } else out.append(match[0]);
    }
    at = match.index + match[0].length;
  }
  if (at < text.length) out.append(text.slice(at));
  return out;
}

const cells = (line) => line.trim().replace(/^\||\|$/g, '').split(/(?<!\\)\|/).map((cell) => cell.trim().replace(/\\\|/g, '|'));

// The Markdown the command writes: headings, paragraphs, lists (nested by
// indentation), tables, fenced code and rules.
function markdown(text) {
  const root = el('div', 'output-md');
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) { i++; continue; }
    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      root.append(el(`h${Math.min(6, heading[1].length + 2)}`, 'output-md__heading', inline(heading[2])));
      i++;
      continue;
    }
    if (/^```/.test(line)) {
      const body = [];
      for (i++; i < lines.length && !/^```/.test(lines[i]); i++) body.push(lines[i]);
      i++;
      root.append(el('pre', 'output-md__pre', el('code', null, body.join('\n'))));
      continue;
    }
    if (/^\s*(\*\s*){3,}$|^\s*(-\s*){3,}$/.test(line)) { root.append(el('hr')); i++; continue; }
    if (/^\s*\|/.test(line) && i + 1 < lines.length && /^\s*\|?\s*:?-{3,}/.test(lines[i + 1])) {
      const table = el('table', 'output-md__table');
      const head = el('tr');
      for (const cell of cells(line)) head.append(el('th', null, inline(cell)));
      table.append(el('thead', null, head));
      const body = el('tbody');
      for (i += 2; i < lines.length && /^\s*\|/.test(lines[i]); i++) {
        const row = el('tr');
        for (const cell of cells(lines[i])) row.append(el('td', null, inline(cell)));
        body.append(row);
      }
      table.append(body);
      root.append(el('div', 'output-md__scroll', table));
      continue;
    }
    if (/^\s*([-*+]|\d+[.)])\s+/.test(line)) {
      const stack = [];
      for (; i < lines.length && /^\s*([-*+]|\d+[.)])\s+/.test(lines[i]); i++) {
        const [, indent, marker, body] = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(lines[i]);
        const depth = Math.floor(indent.length / 2);
        while (stack.length > depth + 1) stack.pop();
        if (stack.length < depth + 1) {
          const list = el(/\d/.test(marker) ? 'ol' : 'ul', 'output-md__list');
          if (stack.length) (stack.at(-1).lastElementChild ?? stack.at(-1)).append(list);
          else root.append(list);
          stack.push(list);
        }
        stack.at(-1).append(el('li', null, inline(body)));
      }
      continue;
    }
    const paragraph = [];
    for (; i < lines.length && lines[i].trim() && !/^(#{1,6}\s|```|\s*\||\s*([-*+]|\d+[.)])\s+)/.test(lines[i]); i++) paragraph.push(lines[i].trim());
    root.append(el('p', null, inline(paragraph.join(' '))));
  }
  return root;
}

// -------------------------------------------------------------------- JSON

function jsonValue(value, depth, label = []) {
  const leaf = (kind, text) => el('span', null, ...label, el('span', `output-tree__${kind}`, text));
  if (value === null) return leaf('null', 'null');
  if (typeof value === 'string') return leaf('string', JSON.stringify(value));
  if (typeof value === 'number') return leaf('number', String(value));
  if (typeof value === 'boolean') return leaf('boolean', String(value));
  const array = Array.isArray(value);
  const entries = array ? value.map((v, i) => [i, v]) : Object.entries(value);
  const [open, close] = array ? ['[', ']'] : ['{', '}'];
  if (!entries.length) return el('span', null, ...label, el('span', 'output-tree__punct', open + close));
  const branch = el('details', 'output-tree__branch');
  branch.open = depth < OPEN_DEPTH;
  // Closed, an object shows its first simple values (which actor it is), an
  // array how many items it holds.
  const simple = array ? [] : entries.filter(([, v]) => v === null || typeof v !== 'object').slice(0, 3);
  const count = simple.length
    ? simple.map(([k, v]) => `${k}: ${JSON.stringify(v)}`).join(', ') + (entries.length > simple.length ? ', …' : '')
    : `${entries.length} ${array ? (entries.length === 1 ? 'item' : 'items') : (entries.length === 1 ? 'key' : 'keys')}`;
  branch.append(el('summary', null, ...label, el('span', 'output-tree__punct', open), el('span', 'output-tree__count', count), el('span', 'output-tree__punct output-tree__close-inline', close)));
  const list = el('ul', 'output-tree__children');
  for (const [key, child] of entries) {
    const name = array ? [] : [el('span', 'output-tree__key', JSON.stringify(String(key))), el('span', 'output-tree__punct', ': ')];
    list.append(el('li', null, jsonValue(child, depth + 1, name)));
  }
  branch.append(list, el('span', 'output-tree__punct output-tree__close', close));
  return branch;
}

function json(text) {
  return el('div', 'output-tree', jsonValue(JSON.parse(text), 0));
}

// JSON lines (the run command's protocol): one tree per line, numbered.
function jsonLines(text) {
  const lines = text.split('\n').filter((line) => line.trim());
  const values = lines.map((line) => JSON.parse(line));
  const list = el('ol', 'output-tree output-tree--lines');
  for (const value of values) list.append(el('li', null, jsonValue(value, 1)));
  return list;
}

// --------------------------------------------------------------------- XML

function xmlNode(node, depth) {
  if (node.nodeType === Node.TEXT_NODE || node.nodeType === Node.CDATA_SECTION_NODE) {
    const text = node.nodeValue.trim();
    return text ? el('span', 'output-tree__text', text) : null;
  }
  if (node.nodeType === Node.COMMENT_NODE) return el('span', 'output-tree__comment', `<!--${node.nodeValue}-->`);
  if (node.nodeType !== Node.ELEMENT_NODE) return null;
  const tag = (closing) => {
    const start = el('span', 'output-tree__tag', closing ? `</${node.nodeName}` : `<${node.nodeName}`);
    const out = [start];
    if (!closing) {
      for (const attribute of node.attributes) {
        out.push(' ', el('span', 'output-tree__key', attribute.name), el('span', 'output-tree__punct', '='),
          el('span', 'output-tree__string', JSON.stringify(attribute.value)));
      }
    }
    out.push(el('span', 'output-tree__tag', closing ? '>' : (node.childNodes.length ? '>' : '/>')));
    return out;
  };
  const children = [...node.childNodes].map((child) => xmlNode(child, depth + 1)).filter(Boolean);
  const onlyText = children.length === 1 && children[0].classList.contains('output-tree__text');
  if (!children.length || onlyText) {
    return el('span', 'output-tree__leaf', ...tag(false), ...(onlyText ? [children[0], ...tag(true)] : []));
  }
  const branch = el('details', 'output-tree__branch');
  branch.open = depth < OPEN_DEPTH + 1;
  branch.append(el('summary', null, ...tag(false)));
  const list = el('ul', 'output-tree__children');
  for (const child of children) list.append(el('li', null, child));
  branch.append(list, el('span', 'output-tree__close', ...tag(true)));
  return branch;
}

function xml(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('not XML');
  const root = doc.documentElement;
  if (root.localName === 'svg') { // a drawing: shown as an image, which runs no script
    const img = el('img', 'output-svg');
    img.alt = '';
    img.src = URL.createObjectURL(new Blob([text], { type: 'image/svg+xml' }));
    return el('div', 'output-svg__frame', img);
  }
  return el('div', 'output-tree', xmlNode(root, 0));
}

const RENDER = { markdown, json, jsonl: jsonLines, xml };

// ----------------------------------------------------------------- the block

class Output extends HTMLElement {
  connectedCallback() {
    if (this.dataset.ready) return;
    const render = RENDER[this.dataset.format];
    const figure = this.closest('figure') ?? this;
    const switcher = figure.querySelector('.output__switch');
    const source = this.querySelector('.copy');
    const code = this.querySelector('pre code');
    if (!render || !switcher || !source || !code) return;
    let preview;
    try {
      preview = el('div', 'output__preview', render(code.textContent));
    } catch {
      return; // not its format after all: the source stays
    }
    preview.tabIndex = 0;
    source.after(preview);
    this.dataset.ready = 'true';
    switcher.hidden = false;
    for (const button of switcher.querySelectorAll('button')) {
      button.addEventListener('click', () => {
        save(button.dataset.view);
        document.dispatchEvent(new CustomEvent(EVENT, { detail: button.dataset.view }));
      });
    }
    this.show(saved() ?? 'preview');
    document.addEventListener(EVENT, (event) => this.show(event.detail));
  }

  show(view) {
    const preview = view !== 'source';
    this.dataset.view = preview ? 'preview' : 'source';
    (this.closest('figure') ?? this).dataset.view = this.dataset.view;
    for (const button of (this.closest('figure') ?? this).querySelectorAll('.output__switch button')) {
      button.setAttribute('aria-pressed', String(button.dataset.view === this.dataset.view));
    }
  }
}

if (!customElements.get('ta-output')) customElements.define('ta-output', Output);
