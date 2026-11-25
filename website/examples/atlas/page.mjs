// Atlas's page: the core's layers and a search on the left; on the right a
// type's bytes on 64-byte cache lines, or a function's callers and callees
// with a drawing of the layers they sit in. atlas.mjs computes every row;
// this module only draws them. Deep links: #type-ta_actor, #fn-ta_host_run.
import { load, layerCounts, layerContents, search, layoutRows, paddingBytes, neighborhood, costOfType, sourceURL, rank, LINE } from './atlas.mjs';

const START = 'type-ta_actor';
const PER_LAYER = 4; // the most callers or callees the drawing shows per layer and side
const number = new Intl.NumberFormat('en');

export async function mount(root) {
  injectStyle();
  const app = el('div', { class: 'atlas' });
  root.append(app);
  let atlas;
  try {
    const response = await fetch(new URL('./atlas.json', import.meta.url));
    if (!response.ok) throw new Error(`atlas.json: ${response.status}`);
    atlas = load(await response.json());
  } catch (error) {
    app.append(el('p', { class: 'atlas__empty' }, `The atlas did not load: ${error.message}`));
    return;
  }

  const input = el('input', { type: 'search', class: 'ta-input atlas__search', placeholder: 'Search types, functions, fields', 'aria-label': 'Search', autocomplete: 'off', spellcheck: 'false' });
  const results = el('ul', { class: 'atlas__results', 'aria-label': 'Results' });
  const layers = el('ul', { class: 'atlas__layers', 'aria-label': 'Layers' });
  const contents = el('div', { class: 'atlas__contents' });
  const side = el('nav', { class: 'atlas__side', 'aria-label': 'Atlas' }, input, results, layers, contents);
  const main = el('section', { class: 'atlas__main', 'aria-live': 'polite', tabindex: '-1' });
  const tip = el('div', { class: 'atlas__tip', role: 'tooltip', hidden: '' });
  const meta = el('p', { class: 'atlas__meta' }, [atlas.meta.commit && `commit ${String(atlas.meta.commit).slice(0, 10)}`, atlas.meta.date, atlas.meta.build].filter(Boolean).join(' · '));
  app.append(side, el('div', { class: 'atlas__body' }, main, meta), tip);

  let openLayer = null;
  for (const { layer, types, functions } of layerCounts(atlas)) {
    const button = el('button', { type: 'button', class: 'atlas__layer', 'aria-expanded': 'false' },
      el('span', { class: 'atlas__layer-name' }, layer), el('span', { class: 'atlas__count' }, `${types} · ${functions}`));
    button.title = `${types} types, ${functions} functions`;
    button.addEventListener('click', () => showLayer(openLayer === layer ? null : layer));
    layers.append(el('li', {}, button));
  }
  function showLayer(layer) {
    openLayer = layer;
    for (const button of layers.querySelectorAll('button')) button.setAttribute('aria-expanded', String(button.firstChild.textContent === layer));
    contents.replaceChildren();
    if (!layer) return;
    const { types, functions } = layerContents(atlas, layer);
    if (types.length) contents.append(el('h4', {}, 'Types'), list(types.map((type) => link(`type-${type.name}`, type.name))));
    if (functions.length) contents.append(el('h4', {}, 'Functions'), list(functions.map((fn) => link(`fn-${fn.name}`, fn.name))));
  }

  input.addEventListener('input', () => {
    results.replaceChildren(...search(atlas, input.value).map((hit) => el('li', {},
      link(hit.kind === 'function' ? `fn-${hit.target}` : hit.kind === 'field' ? `type-${hit.target}:${hit.member}` : `type-${hit.target}`, hit.name),
      el('span', { class: 'atlas__kind' }, hit.kind === 'function' ? 'fn' : hit.kind))));
  });
  input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') results.querySelector('a')?.click();
    if (event.key === 'ArrowDown') { event.preventDefault(); results.querySelector('a')?.focus(); }
    if (event.key === 'Escape') { input.value = ''; results.replaceChildren(); }
  });
  results.addEventListener('keydown', (event) => {
    const links = [...results.querySelectorAll('a')];
    const at = links.indexOf(document.activeElement);
    if (event.key === 'ArrowDown' && at >= 0) { event.preventDefault(); links[Math.min(at + 1, links.length - 1)].focus(); }
    if (event.key === 'ArrowUp' && at >= 0) { event.preventDefault(); (at === 0 ? input : links[at - 1]).focus(); }
  });
  document.addEventListener('keydown', (event) => {
    if (event.key !== '/' || event.ctrlKey || event.metaKey || event.altKey) return;
    const target = event.target;
    if (target instanceof HTMLElement && (target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName))) return;
    if (!app.isConnected) return;
    event.preventDefault();
    input.focus();
  });

  // ---- the tooltip, for a member block hovered or focused
  function showTip(target, lines) {
    tip.replaceChildren(...lines.map((line, i) => el(i ? 'span' : 'strong', {}, line)));
    tip.hidden = false;
    const box = target.getBoundingClientRect();
    const host = app.getBoundingClientRect();
    const width = tip.offsetWidth;
    const left = Math.max(0, Math.min(box.left - host.left + box.width / 2 - width / 2, host.width - width));
    tip.style.left = `${left}px`;
    tip.style.top = `${box.bottom - host.top + 6}px`;
  }
  const hideTip = () => { tip.hidden = true; };

  // ---- the views
  function route() {
    const hash = decodeURIComponent(location.hash.slice(1));
    const [, kind, name, field] = /^(type|fn)-([^:]+)(?::(.+))?$/.exec(hash) ?? /^(type|fn)-([^:]+)(?::(.+))?$/.exec(START);
    hideTip();
    if (kind === 'type' && atlas.typeByName.has(name)) showType(atlas.typeByName.get(name), field);
    else if (kind === 'fn' && atlas.functionByName.has(name)) showFunction(atlas.functionByName.get(name));
    else if (atlas.typeByName.has('ta_actor')) showType(atlas.typeByName.get('ta_actor'));
    else main.replaceChildren(el('p', { class: 'atlas__empty' }, `Nothing named ${name}.`));
  }

  function heading(item, kindLabel) {
    const url = sourceURL(item);
    const where = url ? el('a', { href: url, class: 'atlas__source' }, `${item.path}:${item.line}`) : el('span', {}, item.path ?? '');
    return el('header', { class: 'atlas__head' },
      el('h3', { class: 'atlas__name' }, item.name),
      el('p', { class: 'atlas__facts' }, el('span', { class: 'atlas__badge' }, item.layer), ` ${kindLabel} · `, where));
  }

  function showType(type, field) {
    const padding = paddingBytes(atlas, type);
    const ruler = el('div', { class: 'atlas__ruler', role: 'group', 'aria-label': `${type.name}, ${type.size} bytes on ${LINE}-byte cache lines` });
    const rows = layoutRows(atlas, type);
    // Every member is numbered in offset order; a block shows its name when
    // it fits, else its number, and the key under the ruler names each
    // member shown by number (or too narrow for one), so no name needs a hover.
    const numbers = new Map();
    const labelled = [];
    for (const row of rows) for (const piece of row.pieces) if (piece.span.kind !== 'padding' && !numbers.has(piece.span)) numbers.set(piece.span, numbers.size + 1);
    for (const row of rows) {
      const line = el('div', { class: 'atlas__line' }, el('span', { class: 'atlas__offset', 'aria-hidden': 'true' }, String(row.offset)));
      const track = el('div', { class: 'atlas__track', style: `height:${Math.max(40, 22 * row.lanes)}px` });
      for (const piece of row.pieces) {
        const { span } = piece;
        const lane = 100 / row.lanes;
        const style = `left:${(piece.start / LINE) * 100}%;width:${(Math.max(piece.end - piece.start, 0.25) / LINE) * 100}%;top:calc(${lane * span.lane}% + 2px);height:calc(${lane}% - 4px);bottom:auto`;
        const describe = span.kind === 'padding'
          ? [`${span.size} bytes of padding`, `offset ${span.offset}`]
          : [span.name, `offset ${span.offset} · ${span.bits ? `${span.bits} bits in ` : ''}${span.size} bytes`, span.typeName ?? ''];
        const attributes = { class: `atlas__block atlas__block--${span.kind}${span.described ? ' atlas__block--link' : ''}${span.name === field ? ' is-focus' : ''}`, style, 'aria-label': describe.join(', ') };
        let block;
        if (span.kind === 'padding') block = el('span', { ...attributes, role: 'img' });
        else if (span.described) block = el('a', { ...attributes, href: `#type-${span.described}` });
        else block = el('span', { ...attributes, tabindex: piece.first ? '0' : '-1', role: 'img' });
        if (span.kind !== 'padding' && piece.first) {
          const label = el('span', { class: 'atlas__label' }, span.name);
          block.append(label);
          labelled.push({ block, label, span, n: numbers.get(span) });
        }
        block.addEventListener('pointerenter', () => showTip(block, describe));
        block.addEventListener('pointerleave', hideTip);
        block.addEventListener('focus', () => showTip(block, describe));
        block.addEventListener('blur', hideTip);
        track.append(block);
      }
      if (row.line === rows.length - 1 && type.size % LINE) {
        track.append(el('span', { class: 'atlas__beyond', style: `left:${((type.size % LINE) / LINE) * 100}%` }));
      }
      line.append(track);
      ruler.append(line);
    }
    const table = el('table', { class: 'atlas__table' },
      el('thead', {}, el('tr', {}, ...['Member', 'Offset', 'Size', 'Type'].map((h) => el('th', { scope: 'col' }, h)))),
      el('tbody', {}, ...rowsOf(type).map(([name, offset, size, typeName, target]) => el('tr', { class: name === field ? 'is-focus' : '' },
        el('td', {}, name), el('td', { class: 'atlas__num' }, String(offset)), el('td', { class: 'atlas__num' }, String(size)),
        el('td', {}, target ? link(`type-${target}`, typeName) : typeName ?? '')))));
    const key = el('ol', { class: 'atlas__key', 'aria-label': 'Members too narrow to name on the ruler' });
    const fit = () => {
      const shown = [];
      for (const item of labelled) {
        item.label.textContent = item.span.name;
        const room = item.block.clientWidth - 6;
        if (item.label.scrollWidth <= room) { item.block.dataset.mark = 'name'; continue; }
        item.label.textContent = String(item.n);
        item.block.dataset.mark = item.label.scrollWidth <= room ? 'number' : 'none';
        shown.push(item);
      }
      key.replaceChildren(...shown.map((item) => {
        const entry = el('li', {}, el('b', {}, String(item.n)), ` ${item.span.name} `, el('span', { class: 'atlas__muted' }, `+${item.span.offset}`));
        entry.addEventListener('pointerenter', () => item.block.classList.add('is-hot'));
        entry.addEventListener('pointerleave', () => item.block.classList.remove('is-hot'));
        return entry;
      }));
      key.hidden = !shown.length;
    };
    new ResizeObserver(fit).observe(ruler);
    const parts = [heading(type, `struct · ${number.format(type.size)} bytes${padding ? ` · ${padding} of padding` : ''}`), ruler, key, el('div', { class: 'atlas__scroll' }, table)];
    const containers = atlas.containedIn.get(type.name) ?? [];
    if (containers.length) {
      parts.push(el('h4', {}, 'Contained in'), list(containers.map(({ type: outer, member, offset }) =>
        el('span', {}, link(`type-${outer}:${member}`, `${outer}.${member}`), el('span', { class: 'atlas__muted' }, ` at ${offset}`)))));
    }
    const cost = costOfType(atlas, type);
    if (cost) {
      const bar = el('div', { class: 'atlas__cost', role: 'img', 'aria-label': `One ${cost.name}: ${cost.parts.map((p) => `${p.part} ${p.bytes} bytes`).join(', ')}` });
      cost.parts.forEach(({ part, bytes }, i) => bar.append(el('span', { class: 'atlas__cost-part', style: `flex:${bytes};--part:var(--data-${(i % 6) + 1})`, title: `${part}: ${bytes} bytes` },
        el('span', {}, `${part} ${bytes}`))));
      parts.push(el('h4', {}, `One ${cost.name}: ${number.format(cost.total)} bytes`), bar);
    }
    main.replaceChildren(...parts);
    if (field) main.querySelector('.atlas__block.is-focus')?.scrollIntoView({ block: 'nearest' });
  }

  function rowsOf(type) {
    return [...(type.members ?? [])].sort((a, b) => a[1] - b[1]).map(([name, offset, size, typeName]) => {
      const inner = typeName && !typeName.includes('*') ? typeName.replace(/\[[^\]]*\]/g, '').replace(/\b(struct|const)\b/g, '').trim() : null;
      return [name, offset, size, typeName, inner && inner !== type.name && atlas.typeByName.has(inner) ? inner : null];
    });
  }

  function showFunction(fn) {
    const { callers, callees } = neighborhood(atlas, fn);
    const entry = ({ fn: other, inlined, direction }) => el('span', { class: direction === 'up' ? 'atlas__up' : '' },
      link(`fn-${other.name}`, other.name), el('span', { class: 'atlas__muted' }, ` ${other.layer}${inlined ? ' · inlined' : ''}`));
    main.replaceChildren(
      heading(fn, `function · ${fn.size == null ? 'inlined only' : `${number.format(fn.size)} bytes`}${fn.external ? ' · exported' : ''}`),
      diagram(fn, callers, callees),
      el('div', { class: 'atlas__columns' },
        el('div', {}, el('h4', {}, `Callers (${callers.length})`), callers.length ? list(callers.map(entry)) : el('p', { class: 'atlas__muted' }, 'None in the build.')),
        el('div', {}, el('h4', {}, `Callees (${callees.length})`), callees.length ? list(callees.map(entry)) : el('p', { class: 'atlas__muted' }, 'None.'))));
  }

  // The neighborhood on the layers: one row per layer, host at the top,
  // callers on the left, callees on the right, the function in the middle.
  function diagram(fn, callers, callees) {
    const order = [...atlas.layers].sort((a, b) => (rank(b) ?? -1) - (rank(a) ?? -1));
    // At most a few per layer and side, those that cross a layer first, and
    // one more box counting the rest: the drawing shows the layers reached.
    const pick = (items) => {
      const byLayer = new Map();
      for (const item of [...items].sort((a, b) => (a.direction === 'same') - (b.direction === 'same'))) {
        if (!byLayer.has(item.fn.layer)) byLayer.set(item.fn.layer, []);
        byLayer.get(item.fn.layer).push(item);
      }
      const out = [];
      for (const group of byLayer.values()) {
        out.push(...group.slice(0, PER_LAYER));
        if (group.length > PER_LAYER) out.push({ fn: { name: `${group.length - PER_LAYER} more`, layer: group[0].fn.layer }, more: true, direction: group[0].direction });
      }
      return out;
    };
    const shown = { callers: pick(callers), callees: pick(callees) };
    const used = new Set([fn.layer, ...shown.callers.map((c) => c.fn.layer), ...shown.callees.map((c) => c.fn.layer)]);
    const rows = order.filter((layer) => used.has(layer));
    const rowHeight = 30;
    const columns = { callers: 125, fn: 400, callees: 675 };
    const stacks = new Map();
    const place = (side, layer) => {
      const key = `${side}\u0000${layer}`;
      const n = stacks.get(key) ?? 0;
      stacks.set(key, n + 1);
      return n;
    };
    const nodes = [];
    for (const side of ['callers', 'callees']) for (const item of shown[side]) nodes.push({ side, item, row: rows.indexOf(item.fn.layer), slot: place(side, item.fn.layer) });
    const perRow = rows.map((layer) => Math.max(1, ...['callers', 'callees'].map((side) => stacks.get(`${side}\u0000${layer}`) ?? 0)));
    const tops = [];
    let y = 10;
    for (const count of perRow) { tops.push(y); y += Math.max(rowHeight * count, 36) + 8; }
    const height = y + 2;
    const svg = svgEl('svg', { class: 'atlas__diagram', viewBox: `0 0 800 ${height}`, role: 'img', 'aria-label': `${fn.name} in the ${fn.layer} layer: ${callers.length} callers, ${callees.length} callees; calls up a layer drawn in red` });
    rows.forEach((layer, i) => {
      const band = Math.max(rowHeight * perRow[i], 36);
      svg.append(svgEl('rect', { x: 0, y: tops[i], width: 800, height: band, class: 'atlas__band' }),
        svgText(8, tops[i] + 14, layer, 'atlas__band-label'));
    });
    const center = (row, slot, count = 1) => tops[row] + Math.max(rowHeight * count, 36) / 2 + (slot - (count - 1) / 2) * rowHeight;
    const fnRow = rows.indexOf(fn.layer);
    const fnY = tops[fnRow] + Math.max(rowHeight * perRow[fnRow], 36) / 2;
    for (const node of nodes) {
      const count = stacks.get(`${node.side}\u0000${node.item.fn.layer}`);
      node.y = center(node.row, node.slot, count);
      node.x = columns[node.side];
      if (node.item.more) continue;
      const [x1, y1, x2, y2] = node.side === 'callers' ? [node.x + 75, node.y, columns.fn - 75, fnY] : [columns.fn + 75, fnY, node.x - 75, node.y];
      svg.append(svgEl('path', { d: `M${x1} ${y1} C${(x1 + x2) / 2} ${y1} ${(x1 + x2) / 2} ${y2} ${x2} ${y2}`, class: `atlas__edge${node.item.direction === 'up' ? ' atlas__edge--up' : ''}${node.item.inlined ? ' atlas__edge--inlined' : ''}` }));
    }
    const box = (x, yy, text, cls, href) => {
      const g = svgEl('a', { href, class: cls });
      g.append(svgEl('rect', { x: x - 75, y: yy - 11, width: 150, height: 22, rx: 2 }), svgText(x, yy + 4, clip(text, 22), '', 'middle'));
      const title = svgEl('title', {});
      title.textContent = text;
      g.append(title);
      return g;
    };
    for (const node of nodes) {
      if (node.item.more) svg.append(svgText(node.x, node.y + 4, node.item.fn.name, 'atlas__band-label', 'middle'));
      else svg.append(box(node.x, node.y, node.item.fn.name, `atlas__node${node.item.direction === 'up' ? ' atlas__node--up' : ''}`, `#fn-${node.item.fn.name}`));
    }
    svg.append(box(columns.fn, fnY, fn.name, 'atlas__node atlas__node--self', `#fn-${fn.name}`));
    return el('figure', { class: 'atlas__figure' }, el('div', { class: 'atlas__scroll' }, svg));
  }

  window.addEventListener('hashchange', () => { if (/^#(type|fn)-/.test(location.hash)) { route(); main.focus({ preventScroll: true }); } });
  route();
}

// ---- small helpers
function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) if (value !== null && value !== undefined) node.setAttribute(key, value);
  for (const child of children.flat()) if (child !== null && child !== undefined && child !== false && child !== '') node.append(child);
  return node;
}
function svgEl(tag, attributes = {}) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  return node;
}
function svgText(x, y, text, cls, anchor = 'start') {
  const node = svgEl('text', { x, y, class: cls, 'text-anchor': anchor });
  node.textContent = text;
  return node;
}
const clip = (text, n) => (text.length > n ? `${text.slice(0, n - 1)}…` : text);
const link = (hash, text) => el('a', { href: `#${hash}` }, text);
const list = (items) => el('ul', { class: 'atlas__list' }, ...items.map((item) => el('li', {}, item)));

function injectStyle() {
  if (document.querySelector('#atlas-style')) return;
  const style = el('style', { id: 'atlas-style' });
  style.textContent = `
.atlas { position: relative; display: grid; grid-template-columns: minmax(200px, 248px) minmax(0, 1fr); gap: var(--space-4, 16px);
  background: var(--surface-1); border: 1px solid var(--border-1); border-radius: 2px; padding: var(--space-4, 16px); color: var(--fg-1);
  font-family: var(--font-sans, system-ui); font-size: 14px; min-height: 520px; }
.atlas a { color: var(--accent-fg); text-decoration: none; }
.atlas a:hover { text-decoration: underline; }
.atlas a:focus-visible, .atlas button:focus-visible, .atlas [tabindex]:focus-visible { outline: 2px solid var(--focus-ring); outline-offset: 2px; }
.atlas__side { display: flex; flex-direction: column; gap: var(--space-2, 8px); min-width: 0; border-right: 1px solid var(--border-1); padding-right: var(--space-4, 16px); }
.atlas__search { width: 100%; box-sizing: border-box; height: 32px; padding: 0 8px; font: inherit; color: var(--fg-1); background: var(--bg-app);
  border: 1px solid var(--border-2); border-radius: 2px; }
.atlas__results, .atlas__layers, .atlas__list { list-style: none; margin: 0; padding: 0; }
.atlas__results li { display: flex; justify-content: space-between; gap: 8px; padding: 2px 0; font-family: var(--font-mono, monospace); font-size: 12px; overflow-wrap: anywhere; }
.atlas__results:empty { display: none; }
.atlas__kind { color: var(--fg-3); font-size: 11px; }
.atlas__layer { display: flex; width: 100%; justify-content: space-between; align-items: center; min-height: 28px; padding: 0 8px; border: 0; border-radius: 2px;
  background: transparent; color: var(--fg-1); font: 13px var(--font-mono, monospace); cursor: pointer; }
.atlas__layer:hover { background: var(--surface-2); }
.atlas__layer[aria-expanded="true"] { background: var(--state-running-bg); color: var(--state-running); }
.atlas__count { color: var(--fg-3); font-size: 11px; }
.atlas__contents { max-height: 320px; overflow: auto; font-family: var(--font-mono, monospace); font-size: 12px; }
.atlas__contents:empty { display: none; }
.atlas h4 { margin: var(--space-4, 16px) 0 var(--space-2, 8px); font: 500 11px/1.4 var(--font-mono, monospace); letter-spacing: .12em; text-transform: uppercase; color: var(--fg-3); }
.atlas__contents h4 { margin-top: 8px; }
.atlas__list li { padding: 2px 0; font-family: var(--font-mono, monospace); font-size: 13px; overflow-wrap: anywhere; }
.atlas__body { min-width: 0; display: flex; flex-direction: column; }
.atlas__main { min-width: 0; outline: none; flex: 1; }
.atlas__meta { margin: var(--space-4, 16px) 0 0; color: var(--fg-3); font: 11px var(--font-mono, monospace); }
.atlas__head { margin-bottom: var(--space-3, 12px); }
.atlas__name { margin: 0; font: 600 20px/1.3 var(--font-mono, monospace); overflow-wrap: anywhere; }
.atlas__facts { margin: 4px 0 0; color: var(--fg-2); font-size: 13px; }
.atlas__badge { display: inline-block; padding: 0 6px; border: 1px solid var(--border-2); border-radius: 1px; font: 11px/18px var(--font-mono, monospace); color: var(--fg-2); }
.atlas__source { font-family: var(--font-mono, monospace); font-size: 12px; overflow-wrap: anywhere; }
.atlas__ruler { display: grid; gap: 4px; margin: var(--space-3, 12px) 0; }
.atlas__line { display: grid; grid-template-columns: 36px 1fr; align-items: center; gap: 6px; }
.atlas__offset { color: var(--fg-3); font: 11px var(--font-mono, monospace); text-align: right; }
.atlas__track { position: relative; height: 40px; background: var(--bg-sunken); border: 1px solid var(--border-1);
  background-image: repeating-linear-gradient(90deg, transparent 0 calc(12.5% - 1px), var(--grid-major) calc(12.5% - 1px) 12.5%); }
.atlas__block { position: absolute; top: 2px; bottom: 2px; box-sizing: border-box; border: 1px solid var(--border-strong); background: var(--surface-1);
  overflow: hidden; display: flex; align-items: center; padding: 0 3px; color: var(--fg-1); }
.atlas__block + .atlas__block { margin-left: 1px; }
.atlas__block:hover, .atlas__block:focus-visible { background: var(--surface-2); z-index: 1; }
.atlas__block--link { border-color: var(--accent); background: var(--accent-subtle); color: var(--accent-fg); }
.atlas__block--padding { border: 1px dashed var(--border-2);
  background: repeating-linear-gradient(135deg, transparent 0 3px, var(--border-2) 3px 4px); }
.atlas__block.is-focus { outline: 2px solid var(--accent); outline-offset: 1px; z-index: 2; }
.atlas__beyond { position: absolute; top: 0; bottom: 0; right: 0; background: var(--surface-1); opacity: .7; }
.atlas__label { font: 11px var(--font-mono, monospace); white-space: nowrap; overflow: hidden; pointer-events: none; }
.atlas__block[data-mark="number"] .atlas__label { font-weight: 600; color: var(--fg-2); margin: 0 auto; }
.atlas__block[data-mark="none"] .atlas__label { visibility: hidden; }
.atlas__block.is-hot { outline: 2px solid var(--accent); outline-offset: 1px; z-index: 2; }
.atlas__key { display: flex; flex-wrap: wrap; gap: 2px 14px; margin: calc(-1 * var(--space-1, 4px)) 0 var(--space-3, 12px) 42px; padding: 0; list-style: none; font: 12px var(--font-mono, monospace); color: var(--fg-1); }
.atlas__key[hidden] { display: none; }
.atlas__key b { display: inline-block; min-width: 16px; padding: 0 3px; border: 1px solid var(--border-strong); border-radius: 1px; font-weight: 600; text-align: center; color: var(--fg-2); }
.atlas__tip { position: absolute; z-index: 10; display: grid; gap: 2px; max-width: 280px; padding: 6px 8px; background: var(--surface-1); border: 1px solid var(--border-2);
  border-radius: 2px; box-shadow: var(--shadow-pop, 0 4px 12px rgba(0,0,0,.15)); font: 12px var(--font-mono, monospace); pointer-events: none; }
.atlas__tip[hidden] { display: none; }
.atlas__tip span { color: var(--fg-2); }
.atlas__scroll { overflow-x: auto; }
.atlas__table { width: 100%; border-collapse: collapse; font: 13px var(--font-mono, monospace); }
.atlas__table th { text-align: left; font: 500 11px var(--font-mono, monospace); letter-spacing: .12em; text-transform: uppercase; color: var(--fg-3); padding: 4px 8px; border-bottom: 1px solid var(--border-2); }
.atlas__table td { padding: 3px 8px; border-bottom: 1px solid var(--border-1); white-space: nowrap; }
.atlas__table tr.is-focus td { background: var(--state-running-bg); }
.atlas__num { text-align: right; font-variant-numeric: tabular-nums; }
.atlas__muted { color: var(--fg-3); }
.atlas__up a { color: var(--state-error); }
.atlas__cost { display: flex; height: 30px; gap: 1px; border: 1px solid var(--border-1); }
.atlas__cost-part { background: color-mix(in srgb, var(--part) 35%, var(--surface-1)); border-left: 3px solid var(--part); min-width: 0; display: flex; align-items: center; padding: 0 4px; overflow: hidden; }
.atlas__cost-part span { font: 11px var(--font-mono, monospace); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.atlas__columns { display: grid; grid-template-columns: 1fr 1fr; gap: var(--space-4, 16px); }
.atlas__columns .atlas__list { max-height: 420px; overflow: auto; }
.atlas__figure { margin: var(--space-3, 12px) 0; }
.atlas__diagram { display: block; width: 100%; min-width: 560px; height: auto; font: 11px var(--font-mono, monospace); }
.atlas__band { fill: var(--bg-sunken); stroke: var(--border-1); }
.atlas__band-label { fill: var(--fg-3); }
.atlas__edge { fill: none; stroke: var(--border-strong); stroke-width: 1.25; }
.atlas__edge--inlined { stroke-dasharray: 2 3; }
.atlas__edge--up { stroke: var(--state-error); stroke-width: 1.5; }
.atlas__node rect { fill: var(--surface-1); stroke: var(--border-strong); }
.atlas__node text { fill: var(--fg-1); }
.atlas__node:hover rect, .atlas__node:focus rect { fill: var(--surface-2); }
.atlas__node--up rect { stroke: var(--state-error); }
.atlas__node--self rect { fill: var(--state-running-bg); stroke: var(--state-running); stroke-width: 1.5; }
.atlas__node--self text { fill: var(--state-running); }
.atlas__empty { color: var(--fg-2); }
@media (max-width: 720px) {
  .atlas { grid-template-columns: minmax(0, 1fr); padding: var(--space-3, 12px); }
  .atlas__side { border-right: 0; border-bottom: 1px solid var(--border-1); padding: 0 0 var(--space-3, 12px); }
  .atlas__layers { display: flex; flex-wrap: wrap; gap: 4px; }
  .atlas__layer { width: auto; gap: 8px; border: 1px solid var(--border-1); }
  .atlas__columns { grid-template-columns: 1fr; }
  .atlas__key { margin-left: 0; }
}`;
  document.head.append(style);
}
