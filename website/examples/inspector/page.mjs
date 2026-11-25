// The snapshot inspector's page: open a snapshot pack (or the sample) and
// see each segment's region as the host left it: the memory map, the pools,
// one cell per actor slot, and an actor's record field by field. decode.mjs
// reads the bytes; this module draws them. Nothing leaves the browser.
import { decodePack, actorRecord, fieldRows, stateName, handleSlot, SnapshotError } from './decode.mjs';

const number = new Intl.NumberFormat('en');
const bytesText = (n) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : n >= 10240 ? `${number.format(Math.round(n / 1024))} KB` : `${number.format(n)} B`);
const TABLE_ROWS = 300; // the actor table's rows; the grid shows every slot
// The lifecycle tokens for each actor state (TA_ACTOR_*), by number.
const STATE_COLORS = ['--bg-sunken', '--state-running-bg', '--state-running', '--state-waiting', '--state-waiting', '--state-done', '--state-stopped'];

export async function mount(root) {
  injectStyle();
  const app = el('div', { class: 'insp' });
  root.append(app);
  const fileName = el('span', { class: 'insp__file' }, '');
  const facts = el('dl', { class: 'insp__facts' });
  const picker = el('input', { type: 'file', class: 'insp__picker', accept: '.snapshot,application/octet-stream' });
  const open = el('label', { class: 'ta-btn ta-btn--primary insp__open' }, 'Open a snapshot', picker);
  const sample = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary' }, 'Load the sample');
  const local = el('span', { class: 'insp__note' }, 'Read in your browser; nothing is uploaded.');
  const bar = el('header', { class: 'insp__bar' }, el('div', { class: 'insp__title' }, fileName, facts), el('div', { class: 'insp__actions' }, open, sample, local));
  const segmentsList = el('ul', { class: 'insp__segments', 'aria-label': 'Segments' });
  const main = el('div', { class: 'insp__main' });
  const record = el('aside', { class: 'insp__record', 'aria-label': 'Actor record', 'aria-live': 'polite' });
  const body = el('div', { class: 'insp__body' }, el('nav', { class: 'insp__side', 'aria-label': 'Segments' }, el('h4', {}, 'Segments'), segmentsList), main, record);
  const message = el('div', { class: 'insp__message', role: 'status' });
  const tip = el('div', { class: 'insp__tip', role: 'tooltip', hidden: '' });
  app.append(bar, message, body, tip);

  let pack = null;
  let segment = null;
  let selected = null; // an actor's slot

  // ---- loading
  async function load(bytes, name) {
    try {
      pack = decodePack(new Uint8Array(bytes));
    } catch (error) {
      pack = null;
      showMessage(name, error);
      return;
    }
    message.replaceChildren();
    message.hidden = true;
    body.hidden = false;
    fileName.textContent = name;
    facts.replaceChildren(...[
      ['system', pack.systemID], ['library', pack.library ?? '?'], ['layout', pack.layoutID.toString(16).padStart(16, '0')],
      ['time', `${number.format(pack.time)} ticks`], ['size', bytesText(pack.bytes)],
    ].flatMap(([term, value]) => [el('dt', {}, term), el('dd', {}, value)]));
    const first = pack.segments.find((s) => s.live && s.actors?.length) ?? pack.segments.find((s) => s.live) ?? null;
    selectSegment(first);
  }
  function showMessage(name, error) {
    body.hidden = true;
    message.hidden = false;
    fileName.textContent = name;
    facts.replaceChildren();
    const what = error instanceof SnapshotError ? error.message : `The snapshot did not decode: ${error.message}`;
    message.replaceChildren(el('strong', {}, what),
      el('p', {}, 'Open a format 4 snapshot: a host of this release writes one with snapshot().'));
  }
  async function loadSample() {
    try {
      const response = await fetch(new URL('./sample.snapshot', import.meta.url));
      if (!response.ok) throw new Error(`${response.status}`);
      await load(await response.arrayBuffer(), 'sample.snapshot');
    } catch (error) {
      showMessage('sample.snapshot', new SnapshotError('missing', `The sample did not load (${error.message}). Open a snapshot of your own.`));
    }
  }
  picker.addEventListener('change', async () => {
    const file = picker.files?.[0];
    if (file) await load(await file.arrayBuffer(), file.name);
    picker.value = '';
  });
  sample.addEventListener('click', loadSample);
  app.addEventListener('dragover', (event) => { event.preventDefault(); app.classList.add('is-drop'); });
  app.addEventListener('dragleave', (event) => { if (!app.contains(event.relatedTarget)) app.classList.remove('is-drop'); });
  app.addEventListener('drop', async (event) => {
    event.preventDefault();
    app.classList.remove('is-drop');
    const file = event.dataTransfer?.files?.[0];
    if (file) await load(await file.arrayBuffer(), file.name);
  });

  // ---- the tooltip
  function showTip(target, lines) {
    tip.replaceChildren(...lines.map((line, i) => el(i ? 'span' : 'strong', {}, line)));
    tip.hidden = false;
    const box = target.getBoundingClientRect ? target.getBoundingClientRect() : target;
    const host = app.getBoundingClientRect();
    const width = tip.offsetWidth;
    tip.style.left = `${Math.max(0, Math.min(box.left - host.left + box.width / 2 - width / 2, host.width - width))}px`;
    tip.style.top = `${box.bottom - host.top + 6}px`;
  }
  const hideTip = () => { tip.hidden = true; };
  const tipOn = (node, lines) => {
    node.addEventListener('pointerenter', () => showTip(node, lines));
    node.addEventListener('pointerleave', hideTip);
    node.addEventListener('focus', () => showTip(node, lines));
    node.addEventListener('blur', hideTip);
  };

  // ---- segments
  function selectSegment(next) {
    segment = next;
    selected = null;
    segmentsList.replaceChildren(...pack.segments.map((s) => {
      const button = el('button', { type: 'button', class: 'insp__segment', 'aria-current': String(s === segment), disabled: s.live ? null : '' },
        el('span', {}, `Segment ${s.index}`),
        el('span', { class: 'insp__muted' }, s.live ? `${number.format(s.actors.length)} actors · ${bytesText(s.regionBytes)}` : 'retired'));
      button.addEventListener('click', () => selectSegment(s));
      return el('li', {}, button);
    }));
    drawSegment();
    const firstActor = segment?.actors?.[0];
    selectActor(firstActor ? firstActor.slot : null);
  }

  function drawSegment() {
    hideTip();
    if (!segment) { main.replaceChildren(el('p', { class: 'insp__muted' }, 'No live segment.')); return; }
    // the memory map
    const strip = el('div', { class: 'insp__map', role: 'list', 'aria-label': `Segment ${segment.index}'s region, ${bytesText(segment.regionBytes)}` });
    for (const band of segment.memoryMap) {
      const node = el('span', { class: `insp__band insp__band--${band.kind}`, role: 'listitem', tabindex: '0', style: `flex-grow:${band.bytes}`,
        'aria-label': `${band.name}: offset ${band.offset}, ${band.bytes} bytes` });
      tipOn(node, [band.name, `offset ${number.format(band.offset)} · ${bytesText(band.bytes)}`]);
      strip.append(node);
    }
    // the pools
    const pools = el('div', { class: 'insp__pools' });
    for (const p of segment.pools) {
      const used = p.capacity ? (p.used / p.capacity) * 100 : 0;
      const high = p.capacity ? (p.highWater / p.capacity) * 100 : 0;
      pools.append(el('div', { class: 'insp__pool' },
        el('span', { class: 'insp__pool-name' }, p.name),
        el('span', { class: 'insp__meter', role: 'img', 'aria-label': `${p.name}: ${p.used} used, high water ${p.highWater}, capacity ${p.capacity}` },
          el('span', { class: 'insp__meter-high', style: `width:${high}%` }), el('span', { class: 'insp__meter-used', style: `width:${used}%` })),
        el('span', { class: 'insp__pool-count' }, `${number.format(p.used)} / ${number.format(p.capacity)}`)));
    }
    main.replaceChildren(
      el('h4', {}, 'Memory'), strip,
      el('h4', {}, 'Pools'), pools,
      el('h4', {}, `Actor slots (${number.format(segment.slots.length)})`), slotGrid(), legend(),
      el('h4', {}, 'Actors'), actorTable());
  }

  // ---- the slot grid: one cell per slot, a canvas either way; with more
  // than 4000 slots the cells shrink to a density view.
  let grid = null;
  function slotGrid() {
    const canvas = el('canvas', { class: 'insp__grid', tabindex: '0', role: 'img',
      'aria-label': `One cell per actor slot, colored by state. Arrow keys move between actors, Enter shows one.` });
    const slots = segment.slots;
    grid = { canvas, slots, cell: slots.length > 4000 ? 4 : 12, gap: slots.length > 4000 ? 0 : 2 };
    const draw = () => {
      const width = Math.max(120, canvas.parentElement?.clientWidth || main.clientWidth || 600);
      let { cell, gap } = grid;
      let perRow = Math.max(1, Math.floor(width / (cell + gap)));
      let rows = Math.ceil(slots.length / perRow);
      while (rows * (cell + gap) > 360 && cell > 1) { cell -= 1; gap = 0; perRow = Math.max(1, Math.floor(width / cell)); rows = Math.ceil(slots.length / perRow); }
      const per = rows * cell > 360 ? Math.ceil(slots.length / (perRow * Math.floor(360 / cell))) : 1; // slots a cell
      rows = Math.ceil(slots.length / per / perRow);
      Object.assign(grid, { perRow, rows, per, step: cell + gap, size: cell });
      const ratio = window.devicePixelRatio || 1;
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(Math.max(1, rows) * (cell + gap) * ratio);
      canvas.style.height = `${Math.max(1, rows) * (cell + gap)}px`;
      const paint = canvas.getContext('2d');
      paint.setTransform(ratio, 0, 0, ratio, 0, 0);
      const style = getComputedStyle(app);
      const colors = STATE_COLORS.map((name) => style.getPropertyValue(name).trim() || '#999');
      const free = style.getPropertyValue('--border-1').trim();
      for (let cellIndex = 0; cellIndex * per < slots.length; cellIndex++) {
        let state = null;
        for (let k = cellIndex * per; k < Math.min(slots.length, (cellIndex + 1) * per); k++) {
          const s = slots[k];
          if (s !== null && (state === null || (s !== 1 && state === 1))) state = s; // anything but idle wins a shared cell
        }
        const x = (cellIndex % perRow) * (cell + gap);
        const y = Math.floor(cellIndex / perRow) * (cell + gap);
        paint.fillStyle = state === null ? free : colors[state] ?? colors[0];
        if (state === null && gap) { paint.strokeStyle = free; paint.lineWidth = 1; paint.strokeRect(x + 0.5, y + 0.5, cell - 1, cell - 1); }
        else paint.fillRect(x, y, cell, cell);
      }
      if (selected !== null) {
        const cellIndex = Math.floor(selected / per);
        paint.strokeStyle = style.getPropertyValue('--fg-1').trim();
        paint.lineWidth = 2;
        paint.strokeRect((cellIndex % perRow) * (cell + gap) - 1, Math.floor(cellIndex / perRow) * (cell + gap) - 1, cell + 2, cell + 2);
      }
    };
    grid.draw = draw;
    const slotAt = (event) => {
      const box = canvas.getBoundingClientRect();
      const column = Math.floor((event.clientX - box.left) / grid.step);
      const row = Math.floor((event.clientY - box.top) / grid.step);
      if (column >= grid.perRow) return null;
      const first = (row * grid.perRow + column) * grid.per;
      for (let k = first; k < Math.min(slots.length, first + grid.per); k++) if (slots[k] !== null) return k;
      return first < slots.length ? first : null;
    };
    canvas.addEventListener('click', (event) => { const slot = slotAt(event); if (slot !== null && slots[slot] !== null) selectActor(slot); });
    canvas.addEventListener('pointermove', (event) => {
      const slot = slotAt(event);
      if (slot === null) { hideTip(); return; }
      const actor = segment.actors.find((a) => a.slot === slot);
      showTip({ left: event.clientX - 1, width: 2, bottom: event.clientY + 8 }, actor ? [`slot ${slot}`, `session ${actor.session} · ${stateName(actor.state)}`] : [`slot ${slot}`, 'free']);
    });
    canvas.addEventListener('pointerleave', hideTip);
    canvas.addEventListener('keydown', (event) => {
      const live = segment.actors;
      if (!live.length) return;
      const at = Math.max(0, live.findIndex((a) => a.slot === selected));
      const moves = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: Math.max(1, grid.perRow * grid.per), ArrowUp: -Math.max(1, grid.perRow * grid.per), Home: -Infinity, End: Infinity };
      if (!(event.key in moves)) return;
      event.preventDefault();
      const step = moves[event.key];
      let next;
      if (Math.abs(step) === 1) next = live[Math.min(live.length - 1, Math.max(0, at + step))];
      else if (step === -Infinity) next = live[0];
      else if (step === Infinity) next = live[live.length - 1];
      else {
        const target = (selected ?? 0) + step;
        next = step > 0 ? live.find((a) => a.slot >= target) ?? live[live.length - 1] : [...live].reverse().find((a) => a.slot <= target) ?? live[0];
      }
      selectActor(next.slot);
    });
    requestAnimationFrame(draw);
    return el('div', { class: 'insp__grid-wrap' }, canvas);
  }
  function legend() {
    const used = new Set(segment.slots.filter((s) => s !== null));
    return el('p', { class: 'insp__legend' }, ...[...used].sort((a, b) => a - b).map((state) =>
      el('span', {}, el('i', { style: `background:var(${STATE_COLORS[state] ?? '--fg-3'})` }), stateName(state))));
  }

  // ---- the actor table
  let table = null;
  function actorTable() {
    const actors = [...segment.actors].sort((a, b) => (a.session < b.session ? -1 : a.session > b.session ? 1 : 0)).slice(0, TABLE_ROWS);
    table = el('tbody', {}, ...actors.map((actor) => {
      const button = el('button', { type: 'button', class: 'insp__link' }, String(actor.session));
      button.addEventListener('click', () => selectActor(actor.slot));
      return el('tr', { 'data-slot': String(actor.slot) },
        el('td', {}, button), el('td', { class: 'insp__num' }, String(actor.slot)),
        el('td', {}, el('span', { class: 'insp__dot', style: `background:var(${STATE_COLORS[actor.state] ?? '--fg-3'})` }), stateName(actor.state)),
        el('td', { class: 'insp__num' }, String(actor.mailboxLength ?? '')), el('td', { class: 'insp__num' }, String(actor.timers ?? '')));
    }));
    const more = segment.actors.length > TABLE_ROWS ? el('p', { class: 'insp__muted' }, `The first ${TABLE_ROWS} of ${number.format(segment.actors.length)} by session; pick others on the grid.`) : '';
    return el('div', {}, el('div', { class: 'insp__scroll' }, el('table', { class: 'insp__table' },
      el('thead', {}, el('tr', {}, ...['Session', 'Slot', 'State', 'Queued', 'Timers'].map((h) => el('th', { scope: 'col' }, h)))), table)), more);
  }

  // ---- one actor's record
  function selectActor(slot) {
    selected = slot;
    grid?.draw?.();
    for (const row of table?.querySelectorAll('tr') ?? []) row.classList.toggle('is-selected', row.dataset.slot === String(slot));
    const actor = segment?.actors?.find((a) => a.slot === slot);
    if (!actor) { record.replaceChildren(el('p', { class: 'insp__muted' }, segment ? 'Pick an actor.' : '')); return; }
    const pools = segment.pools;
    const actorsPool = pools.find((p) => p.name === 'actors');
    const { offset, rows } = actorRecord(pack.layout, segment.region, actorsPool, slot);
    const nil = pack.layout.nil;
    const valueCell = (row) => {
      const value = row.value;
      const pool = /^(index|handle):(.+)$/.exec(row.kind ?? '');
      if (pool && typeof value === 'number') {
        const target = pool[1] === 'handle' ? handleSlot(value) : value === nil ? null : value;
        if (target === null) return el('span', { class: 'insp__muted' }, pool[1] === 'handle' ? '0 (none)' : 'nil');
        if (pool[2] === 'actors' && segment.actors.some((a) => a.slot === target)) {
          const button = el('button', { type: 'button', class: 'insp__link' }, `actors[${target}]`);
          button.addEventListener('click', () => selectActor(target));
          return button;
        }
        return el('span', {}, `${pool[2]}[${target}]`);
      }
      if (row.name === 'state') return el('span', {}, `${value} · ${stateName(value)}`);
      if (Array.isArray(value)) return el('span', {}, value.join(' '));
      if (typeof value === 'bigint') return el('span', {}, value.toString());
      if (value && typeof value === 'object') return el('span', { class: 'insp__muted' }, `${value.bytes ?? '?'} bytes`);
      return el('span', {}, String(value));
    };
    const fields = el('table', { class: 'insp__table insp__fields' },
      el('thead', {}, el('tr', {}, ...['Offset', 'Field', 'Value'].map((h) => el('th', { scope: 'col' }, h)))),
      el('tbody', {}, ...rows.map((row) => el('tr', { title: `${row.type}${row.kind ? ` (${row.kind})` : ''}, ${row.size} bytes` },
        el('td', { class: 'insp__num' }, `+${row.offset}`), el('td', {}, row.name), el('td', {}, valueCell(row))))));
    const parts = [el('h4', {}, `Actor ${actor.session}`), el('p', { class: 'insp__muted' }, `slot ${slot} · ${actorsPool.element} at ${number.format(offset)} · ${actorsPool.stride} bytes`), fields];
    const eventsPool = pools.find((p) => p.name === 'events');
    if (actor.mailbox.length && eventsPool) {
      const view = new DataView(segment.region.buffer, segment.region.byteOffset, segment.region.byteLength);
      parts.push(el('h4', {}, `Mailbox (${actor.mailbox.length})`), el('ol', { class: 'insp__mailbox' }, ...actor.mailbox.map((index) => {
        const eventRows = fieldRows(pack.layout, view, eventsPool.offset + index * eventsPool.stride, eventsPool.element);
        return el('li', {}, el('details', {}, el('summary', {}, `events[${index}]`),
          el('dl', { class: 'insp__event' }, ...eventRows.flatMap((row) => [el('dt', {}, row.name), el('dd', {}, Array.isArray(row.value) ? row.value.join(' ') : String(row.value))]))));
      })));
    }
    record.replaceChildren(...parts);
  }

  new ResizeObserver(() => grid?.draw?.()).observe(main);
  await loadSample();
}

// ---- helpers
function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) if (value !== null && value !== undefined) node.setAttribute(key, value);
  for (const child of children.flat()) if (child !== null && child !== undefined && child !== false && child !== '') node.append(child);
  return node;
}

function injectStyle() {
  if (document.querySelector('#inspector-style')) return;
  const style = el('style', { id: 'inspector-style' });
  style.textContent = `
.insp { position: relative; display: grid; gap: var(--space-3, 12px); background: var(--surface-1); border: 1px solid var(--border-1); border-radius: 2px;
  padding: var(--space-4, 16px); color: var(--fg-1); font: 14px var(--font-sans, system-ui); min-height: 480px; align-content: start; }
.insp.is-drop { outline: 2px dashed var(--accent); outline-offset: -6px; }
.insp h4 { margin: var(--space-4, 16px) 0 var(--space-2, 8px); font: 500 11px/1.4 var(--font-mono, monospace); letter-spacing: .12em; text-transform: uppercase; color: var(--fg-3); }
.insp button:focus-visible, .insp [tabindex]:focus-visible, .insp__open:focus-within { outline: 2px solid var(--focus-ring); outline-offset: 2px; }
.insp__bar { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: var(--space-3, 12px); padding-bottom: var(--space-3, 12px); border-bottom: 1px solid var(--border-1); }
.insp__title { display: grid; gap: 4px; min-width: 0; }
.insp__file { font: 600 16px var(--font-mono, monospace); overflow-wrap: anywhere; }
.insp__facts { display: flex; flex-wrap: wrap; gap: 2px 12px; margin: 0; font: 12px var(--font-mono, monospace); }
.insp__facts dt { color: var(--fg-3); }
.insp__facts dd { margin: 0 0 0 -8px; overflow-wrap: anywhere; }
.insp__actions { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
.insp__picker { position: absolute; width: 1px; height: 1px; opacity: 0; overflow: hidden; }
.insp__open { cursor: pointer; }
.insp__note { color: var(--fg-3); font-size: 12px; }
.insp__message { padding: var(--space-4, 16px); border: 1px solid var(--state-waiting); background: var(--state-waiting-bg); border-radius: 2px; }
.insp__message[hidden] { display: none; }
.insp__message p { margin: 6px 0 0; color: var(--fg-2); }
.insp__body { display: grid; grid-template-columns: 170px minmax(0, 1fr) minmax(240px, 300px); gap: var(--space-4, 16px); }
.insp__body[hidden] { display: none; }
.insp__side h4 { margin-top: 0; }
.insp__segments { list-style: none; margin: 0; padding: 0; display: grid; gap: 4px; }
.insp__segment { display: grid; width: 100%; text-align: left; gap: 2px; padding: 6px 8px; border: 1px solid var(--border-1); border-radius: 2px; background: transparent; color: var(--fg-1);
  font: 13px var(--font-mono, monospace); cursor: pointer; }
.insp__segment:hover { background: var(--surface-2); }
.insp__segment[aria-current="true"] { background: var(--state-running-bg); border-color: var(--state-running); }
.insp__segment:disabled { opacity: .45; cursor: not-allowed; }
.insp__muted { color: var(--fg-3); font-size: 12px; }
.insp__main { min-width: 0; }
.insp__main > h4:first-child { margin-top: 0; }
.insp__map { display: flex; height: 28px; gap: 1px; border: 1px solid var(--border-1); background: var(--border-1); }
.insp__band { min-width: 3px; flex-basis: 0; background: var(--surface-2); }
.insp__band--header { background: var(--fg-3); }
.insp__band--pool { background: var(--accent-subtle); box-shadow: inset 0 0 0 1px var(--accent); }
.insp__band--generations { background: var(--data-1); }
.insp__band--array { background: var(--data-2); }
.insp__band--gap { background: repeating-linear-gradient(135deg, var(--surface-1) 0 3px, var(--border-2) 3px 4px); }
.insp__band:hover, .insp__band:focus-visible { filter: brightness(.85); }
.insp__pools { display: grid; gap: 6px; }
.insp__pool { display: grid; grid-template-columns: 90px minmax(0, 1fr) auto; align-items: center; gap: 8px; font: 12px var(--font-mono, monospace); }
.insp__pool-count { color: var(--fg-2); font-variant-numeric: tabular-nums; }
.insp__meter { position: relative; height: 12px; background: var(--bg-sunken); border: 1px solid var(--border-1); }
.insp__meter-high { position: absolute; inset: 0 auto 0 0; background: var(--accent-subtle); }
.insp__meter-used { position: absolute; inset: 0 auto 0 0; background: var(--accent); }
.insp__grid-wrap { width: 100%; }
.insp__grid { display: block; width: 100%; cursor: pointer; }
.insp__legend { display: flex; flex-wrap: wrap; gap: 4px 14px; margin: 8px 0 0; font-size: 12px; color: var(--fg-2); }
.insp__legend i, .insp__dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; margin-right: 6px; vertical-align: baseline; }
.insp__scroll { overflow: auto; max-height: 360px; }
.insp__table { width: 100%; border-collapse: collapse; font: 12px var(--font-mono, monospace); }
.insp__table th { position: sticky; top: 0; background: var(--surface-1); text-align: left; font: 500 11px var(--font-mono, monospace); letter-spacing: .12em; text-transform: uppercase; color: var(--fg-3); padding: 4px 6px; border-bottom: 1px solid var(--border-2); }
.insp__table td { padding: 3px 6px; border-bottom: 1px solid var(--border-1); white-space: nowrap; }
.insp__table tr.is-selected td { background: var(--state-running-bg); }
.insp__num { text-align: right; font-variant-numeric: tabular-nums; color: var(--fg-2); }
.insp__link { padding: 0; border: 0; background: none; color: var(--accent-fg); font: inherit; cursor: pointer; text-decoration: underline; text-decoration-color: var(--border-2); }
.insp__record { min-width: 0; border-left: 1px solid var(--border-1); padding-left: var(--space-4, 16px); }
.insp__record h4:first-child { margin-top: 0; }
.insp__fields td:last-child { white-space: normal; overflow-wrap: anywhere; }
.insp__mailbox { margin: 0; padding-left: 20px; font: 12px var(--font-mono, monospace); }
.insp__mailbox summary { cursor: pointer; color: var(--accent-fg); }
.insp__event { display: grid; grid-template-columns: auto 1fr; gap: 0 10px; margin: 4px 0 8px; }
.insp__event dt { color: var(--fg-3); }
.insp__event dd { margin: 0; overflow-wrap: anywhere; }
.insp__tip { position: absolute; z-index: 10; display: grid; gap: 2px; max-width: 280px; padding: 6px 8px; background: var(--surface-1); border: 1px solid var(--border-2);
  border-radius: 2px; box-shadow: var(--shadow-pop, 0 4px 12px rgba(0,0,0,.15)); font: 12px var(--font-mono, monospace); pointer-events: none; }
.insp__tip[hidden] { display: none; }
.insp__tip span { color: var(--fg-2); }
@media (max-width: 960px) {
  .insp__body { grid-template-columns: 150px minmax(0, 1fr); }
  .insp__record { grid-column: 1 / -1; border-left: 0; padding-left: 0; border-top: 1px solid var(--border-1); padding-top: var(--space-3, 12px); }
}
@media (max-width: 640px) {
  .insp { padding: var(--space-3, 12px); }
  .insp__body { grid-template-columns: minmax(0, 1fr); }
  .insp__segments { display: flex; flex-wrap: wrap; }
  .insp__segment { width: auto; }
}`;
  document.head.append(style);
}
