// The snapshot inspector: a devtool over a snapshot pack (or the sample).
// One toolbar, then three panes that fill the window and scroll on their
// own: the navigator (segments, pools, every actor), the memory (the
// region's map and one cell per actor slot) and the inspector (the
// selected record field by field, its links followed with Back). On a
// phone one pane shows at a time. decode.mjs reads the bytes; this module
// draws them. Nothing leaves the browser.
import * as decode from './decode.mjs';

const { decodePack, actorRecord, fieldRows, stateName, handleSlot, SnapshotError } = decode;
const number = new Intl.NumberFormat('en');
const bytesText = (n) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : n >= 10240 ? `${number.format(Math.round(n / 1024))} KB` : `${number.format(n)} B`);
const ROW = 28; // the actor list's row height, in pixels
// The color of each actor state (TA_ACTOR_*), by number: every state
// distinct in both themes, idle quiet but visible.
const STATE_COLORS = ['--border-2', '--data-5', '--state-running', '--state-waiting', '--data-4', '--state-done', '--data-3'];
const stateColor = (state) => `var(${STATE_COLORS[state] ?? '--fg-3'})`;
const PHONE = '(max-width: 720px)';

export async function mount(root) {
  injectStyle();
  const app = el('div', { class: 'insp', 'data-view': 'actors' });
  root.append(app);

  // ---- the toolbar
  const fileName = el('span', { class: 'insp__file' }, 'No snapshot');
  const facts = el('dl', { class: 'insp__facts' });
  const picker = el('input', { type: 'file', class: 'insp__picker', accept: '.snapshot,application/octet-stream', 'aria-label': 'Open a snapshot' });
  const open = el('label', { class: 'ta-btn ta-btn--primary ta-btn--sm insp__open' }, 'Open a snapshot', picker);
  const sample = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Load the sample');
  const local = el('span', { class: 'insp__note' }, 'Read locally, never uploaded');
  const bar = el('header', { class: 'insp__bar' }, el('div', { class: 'insp__title' }, fileName, facts), el('div', { class: 'insp__actions' }, local, open, sample));

  // ---- the phone's pane switch
  const views = [['actors', 'Actors'], ['memory', 'Memory'], ['record', 'Record']];
  const switcher = el('div', { class: 'insp__switch', role: 'tablist', 'aria-label': 'Pane' }, ...views.map(([value, label]) => {
    const button = el('button', { type: 'button', role: 'tab', 'data-view': value, 'aria-selected': String(value === 'actors') }, label);
    button.addEventListener('click', () => show(value));
    return button;
  }));
  const show = (view) => {
    app.dataset.view = view;
    for (const button of switcher.children) button.setAttribute('aria-selected', String(button.dataset.view === view));
    if (view === 'memory') requestAnimationFrame(() => grid.draw());
    if (view === 'actors') requestAnimationFrame(() => list.draw());
  };

  // ---- the navigator
  const segmentsList = el('ul', { class: 'insp__segments' });
  const poolsList = el('div', { class: 'insp__pools' });
  const filter = el('input', { type: 'search', class: 'insp__filter', placeholder: 'Filter by session, state or name   /', 'aria-label': 'Filter actors', autocomplete: 'off', spellcheck: 'false' });
  const actorCount = el('span', { class: 'insp__count' });
  const listHead = el('div', { class: 'insp__list-head', 'aria-hidden': 'true' }, ...['Session', 'State', 'Queued', 'Timers'].map((h) => el('span', {}, h)));
  const listScroll = el('div', { class: 'insp__list', role: 'listbox', tabindex: '0', 'aria-label': 'Actors' });
  const listSpacer = el('div', { class: 'insp__list-spacer' });
  listScroll.append(listSpacer);
  const nav = el('nav', { class: 'insp__pane insp__nav', 'aria-label': 'Navigator' },
    el('section', { class: 'insp__group' }, el('h2', {}, 'Segments'), segmentsList),
    el('section', { class: 'insp__group' }, el('h2', {}, 'Pools'), poolsList),
    el('section', { class: 'insp__group insp__group--grow' }, el('h2', {}, 'Actors ', actorCount), filter, listHead, listScroll));

  // ---- the memory
  const strip = el('div', { class: 'insp__map', role: 'list' });
  const legend = el('p', { class: 'insp__legend' });
  const canvas = el('canvas', { class: 'insp__grid', tabindex: '0', role: 'img', 'aria-label': 'One cell per actor slot, colored by state. Arrow keys move between actors.' });
  const gridScroll = el('div', { class: 'insp__grid-scroll' }, canvas);
  const bands = el('tbody');
  const bandsTable = el('div', { class: 'insp__bands' }, el('table', { class: 'insp__fields' },
    el('thead', {}, el('tr', {}, ...['Offset', 'Band', 'Size'].map((h) => el('th', { scope: 'col' }, h)))), bands));
  const center = el('section', { class: 'insp__pane insp__memory', 'aria-label': 'Memory' },
    el('h2', {}, 'Region'), strip, el('h2', {}, 'Actor slots'), legend, gridScroll, el('h2', {}, 'Region map'), bandsTable);

  // ---- the inspector
  const crumbs = el('div', { class: 'insp__crumbs' });
  const back = el('button', { type: 'button', class: 'ta-btn ta-btn--ghost ta-btn--sm insp__back', disabled: '' }, '← Back');
  const recordBody = el('div', { class: 'insp__record-body', 'aria-live': 'polite' });
  const inspector = el('aside', { class: 'insp__pane insp__inspector', 'aria-label': 'Record' },
    el('div', { class: 'insp__crumbbar' }, back, crumbs), recordBody);

  const panes = el('div', { class: 'insp__panes' }, nav, center, inspector);
  const message = el('div', { class: 'insp__message', role: 'alert', hidden: '' });
  const status = el('footer', { class: 'insp__status', role: 'status' });
  const tip = el('div', { class: 'insp__tip', role: 'tooltip', hidden: '' });
  app.append(bar, switcher, message, panes, status, tip);

  let pack = null;
  let segment = null;
  let shown = []; // the actors the filter keeps, by session
  let cursor = -1; // the list's highlighted row, in `shown`
  let history = []; // the inspector's trail: { pool, index }
  const selectedSlot = () => { const top = history[0]; return top && top.pool === 'actors' ? top.index : null; };

  // ---- loading
  async function load(bytes, name) {
    try {
      pack = decodePack(new Uint8Array(bytes));
    } catch (error) {
      pack = null;
      fail(name, error);
      return;
    }
    message.hidden = true;
    panes.hidden = false;
    fileName.textContent = name;
    facts.replaceChildren(...[
      ['system', pack.systemID], ['library', pack.library ?? '?'], ['layout', pack.layoutID.toString(16).padStart(16, '0')],
      ['time', `${number.format(pack.time)} ticks`], ['size', bytesText(pack.bytes)],
    ].flatMap(([term, value]) => [el('dt', {}, term), el('dd', {}, value)]));
    selectSegment(pack.segments.find((s) => s.live && s.actors?.length) ?? pack.segments.find((s) => s.live) ?? null);
  }
  function fail(name, error) {
    panes.hidden = true;
    message.hidden = false;
    fileName.textContent = name;
    facts.replaceChildren();
    status.textContent = '';
    const what = error instanceof SnapshotError ? error.message : `${name} is not a snapshot this page can read: ${error.message}`;
    message.replaceChildren(el('strong', {}, what),
      el('p', {}, 'Open a format 4 snapshot, which host.snapshot() writes in this release, or load the sample.'));
  }
  async function loadSample() {
    try {
      const response = await fetch(new URL('./sample.snapshot', import.meta.url));
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      await load(await response.arrayBuffer(), 'sample.snapshot');
    } catch (error) {
      fail('sample.snapshot', new SnapshotError('missing', `The sample did not load (${error.message}). Reload the page, or open a snapshot of your own.`));
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
  function showTip(box, lines) {
    tip.replaceChildren(...lines.map((line, i) => el(i ? 'span' : 'strong', {}, line)));
    tip.hidden = false;
    const host = app.getBoundingClientRect();
    const width = tip.offsetWidth;
    tip.style.left = `${Math.max(0, Math.min(box.left - host.left + box.width / 2 - width / 2, host.width - width))}px`;
    tip.style.top = `${box.bottom - host.top + 6}px`;
  }
  const hideTip = () => { tip.hidden = true; };

  // ---- a segment
  function selectSegment(next) {
    segment = next;
    history = [];
    segmentsList.replaceChildren(...pack.segments.map((s) => {
      const button = el('button', { type: 'button', class: 'insp__segment', 'aria-current': String(s === segment), disabled: s.live ? null : '' },
        el('span', {}, `Segment ${s.index}`),
        el('span', { class: 'insp__muted' }, s.live ? `${number.format(s.actors.length)} actors · ${bytesText(s.regionBytes)}` : 'retired'));
      button.addEventListener('click', () => selectSegment(s));
      return el('li', {}, button);
    }));
    drawPools();
    drawStrip();
    drawLegend();
    applyFilter();
    const first = shown[0];
    if (first) openRecord({ pool: 'actors', index: first.slot }, { reset: true });
    else { renderRecord(); grid.draw(); }
  }

  function drawPools() {
    poolsList.replaceChildren(...(segment?.pools ?? []).map((p) => {
      const used = p.capacity ? (p.used / p.capacity) * 100 : 0;
      const high = p.capacity ? (p.highWater / p.capacity) * 100 : 0;
      return el('div', { class: 'insp__pool', title: `${p.name}: ${p.used} used, high water ${p.highWater}, capacity ${p.capacity}, ${p.stride} bytes a slot` },
        el('span', { class: 'insp__pool-name' }, p.name),
        el('span', { class: 'insp__meter', role: 'img', 'aria-label': `${p.name}: ${p.used} used of ${p.capacity}, high water ${p.highWater}` },
          el('span', { class: 'insp__meter-high', style: `width:${high}%` }), el('span', { class: 'insp__meter-used', style: `width:${used}%` })),
        el('span', { class: 'insp__pool-count' }, `${number.format(p.used)}/${number.format(p.capacity)}`));
    }));
  }

  function drawStrip() {
    hideTip();
    strip.replaceChildren();
    bands.replaceChildren();
    if (!segment) return;
    strip.setAttribute('aria-label', `Segment ${segment.index}'s region, ${bytesText(segment.regionBytes)}`);
    for (const band of segment.memoryMap) {
      const node = el('span', { class: `insp__band insp__band--${band.kind}`, role: 'listitem', tabindex: '0', style: `flex-grow:${band.bytes}`,
        'aria-label': `${band.name}: offset ${band.offset}, ${band.bytes} bytes` });
      const lines = [band.name, `offset ${number.format(band.offset)} · ${bytesText(band.bytes)}`];
      node.addEventListener('pointerenter', () => showTip(node.getBoundingClientRect(), lines));
      node.addEventListener('pointerleave', hideTip);
      node.addEventListener('focus', () => showTip(node.getBoundingClientRect(), lines));
      node.addEventListener('blur', hideTip);
      const pool = segment.pools.find((p) => p.name === band.name);
      if (pool && pool.used) node.addEventListener('click', () => {
        const index = firstLive(pool);
        if (index !== null) openRecord({ pool: pool.name, index }, { reset: true });
      });
      strip.append(node);
      const index = pool && pool.used ? firstLive(pool) : null;
      bands.append(el('tr', {}, el('td', { class: 'insp__num' }, number.format(band.offset)),
        el('td', {}, index !== null ? linkButton({ pool: pool.name, index }, band.name) : el('span', { class: band.kind === 'gap' ? 'insp__muted' : null }, band.name)),
        el('td', { class: 'insp__num' }, bytesText(band.bytes))));
    }
  }
  function drawLegend() {
    const used = new Set((segment?.slots ?? []).map((s) => (s === null ? 0 : s)));
    legend.replaceChildren(...[...used].sort((a, b) => a - b).map((state) =>
      el('span', {}, el('i', { class: state === 0 ? 'is-free' : null, style: `--dot:${stateColor(state)}` }), stateName(state))));
  }
  const firstLive = (pool) => (pool.name === 'actors' ? (segment.actors[0]?.slot ?? null) : pool.used ? 0 : null);

  // ---- the actor list: filtered, and only the rows in view are drawn
  const labelOf = (actor) => (typeof actor.name === 'string' ? actor.name : null) ?? actor.strings?.name ?? null;
  function applyFilter() {
    const query = filter.value.trim().toLowerCase();
    const all = [...(segment?.actors ?? [])].sort((a, b) => (a.session < b.session ? -1 : a.session > b.session ? 1 : 0));
    shown = !query ? all : all.filter((a) => [String(a.session), `slot ${a.slot}`, stateName(a.state), labelOf(a), a.eventName]
      .some((text) => text && String(text).toLowerCase().includes(query)));
    actorCount.textContent = query ? `${number.format(shown.length)} of ${number.format(all.length)}` : number.format(all.length);
    cursor = shown.findIndex((a) => a.slot === selectedSlot());
    listSpacer.style.height = `${shown.length * ROW}px`;
    list.draw();
    updateStatus();
  }
  const list = {
    draw() {
      const top = listScroll.scrollTop;
      const height = listScroll.clientHeight || 400;
      const from = Math.max(0, Math.floor(top / ROW) - 8);
      const to = Math.min(shown.length, Math.ceil((top + height) / ROW) + 8);
      const rows = [];
      if (!shown.length) rows.push(el('p', { class: 'insp__empty' }, segment ? 'No actor matches the filter.' : 'Open a snapshot to list its actors.'));
      for (let i = from; i < to; i++) {
        const actor = shown[i];
        const label = labelOf(actor);
        const row = el('div', { class: `insp__row${i === cursor ? ' is-cursor' : ''}${actor.slot === selectedSlot() ? ' is-selected' : ''}`, role: 'option',
          'aria-selected': String(actor.slot === selectedSlot()), style: `top:${i * ROW}px`, 'data-i': String(i) },
          el('span', { class: 'insp__session' }, String(actor.session), label ? el('span', { class: 'insp__label' }, label) : null),
          el('span', { class: 'insp__state' }, el('i', { style: `--dot:${stateColor(actor.state)}` }), stateName(actor.state)),
          el('span', { class: 'insp__num' }, actor.mailboxLength ? String(actor.mailboxLength) : '·'),
          el('span', { class: 'insp__num' }, actor.timers ? String(actor.timers) : '·'));
        rows.push(row);
      }
      listSpacer.replaceChildren(...rows);
    },
  };
  listScroll.addEventListener('scroll', () => list.draw(), { passive: true });
  listScroll.addEventListener('click', (event) => {
    const row = event.target.closest('.insp__row');
    if (!row) return;
    cursor = Number(row.dataset.i);
    openActor(shown[cursor], true);
  });
  filter.addEventListener('input', () => { listScroll.scrollTop = 0; applyFilter(); });
  function moveCursor(step) {
    if (!shown.length) return;
    cursor = Math.min(shown.length - 1, Math.max(0, (cursor < 0 ? (step > 0 ? -1 : shown.length) : cursor) + step));
    const top = cursor * ROW;
    if (top < listScroll.scrollTop) listScroll.scrollTop = top;
    else if (top + ROW > listScroll.scrollTop + listScroll.clientHeight) listScroll.scrollTop = top + ROW - listScroll.clientHeight;
    openActor(shown[cursor], false);
  }
  function openActor(actor, reveal) {
    if (!actor) return;
    openRecord({ pool: 'actors', index: actor.slot }, { reset: true });
    if (reveal && matchMedia(PHONE).matches) show('record');
  }

  // ---- the slot grid: one cell per slot, on a canvas; past 4000 slots
  // the cells shrink to a density view.
  const grid = { cell: 12, gap: 2, perRow: 1, per: 1, step: 14 };
  grid.draw = () => {
    const slots = segment?.slots ?? [];
    const width = Math.max(120, gridScroll.clientWidth - 2);
    // cells as large as the width allows in about five rows, 8 to 22 px; past 4000 slots, a density view
    let cell = slots.length > 4000 ? 4 : Math.max(8, Math.min(22, Math.floor(width / Math.max(1, Math.ceil(slots.length / 5))) - 2));
    let gap = slots.length > 4000 ? 0 : 2;
    let perRow = Math.max(1, Math.floor(width / (cell + gap)));
    let rows = Math.ceil(slots.length / perRow);
    while (rows * (cell + gap) > 2000 && cell > 1) { cell -= 1; gap = 0; perRow = Math.max(1, Math.floor(width / cell)); rows = Math.ceil(slots.length / perRow); }
    const per = rows * cell > 2000 ? Math.ceil(slots.length / (perRow * Math.floor(2000 / cell))) : 1; // slots a cell
    rows = Math.ceil(slots.length / per / perRow);
    Object.assign(grid, { perRow, per, step: cell + gap, cell });
    const ratio = window.devicePixelRatio || 1;
    const height = Math.max(1, rows) * (cell + gap);
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
    canvas.style.width = `${width}px`;
    canvas.style.height = `${height}px`;
    const paint = canvas.getContext('2d');
    paint.setTransform(ratio, 0, 0, ratio, 0, 0);
    paint.clearRect(0, 0, width, height);
    const style = getComputedStyle(app);
    const colors = STATE_COLORS.map((name) => style.getPropertyValue(name).trim() || '#999');
    for (let cellIndex = 0; cellIndex * per < slots.length; cellIndex++) {
      let state = null;
      for (let k = cellIndex * per; k < Math.min(slots.length, (cellIndex + 1) * per); k++) {
        const s = slots[k];
        if (s !== null && (state === null || (s !== 1 && state === 1))) state = s; // anything but idle wins a shared cell
      }
      const x = (cellIndex % perRow) * (cell + gap);
      const y = Math.floor(cellIndex / perRow) * (cell + gap);
      if (state === null && gap) { paint.strokeStyle = colors[0]; paint.lineWidth = 1; paint.strokeRect(x + 0.5, y + 0.5, cell - 1, cell - 1); }
      else { paint.fillStyle = colors[state ?? 0] ?? colors[0]; paint.fillRect(x, y, cell, cell); }
    }
    const selected = selectedSlot();
    if (selected !== null) {
      const cellIndex = Math.floor(selected / per);
      paint.strokeStyle = style.getPropertyValue('--fg-1').trim();
      paint.lineWidth = 2;
      paint.strokeRect((cellIndex % perRow) * (cell + gap) - 1, Math.floor(cellIndex / perRow) * (cell + gap) - 1, cell + 2, cell + 2);
    }
  };
  const slotAt = (event) => {
    const slots = segment?.slots ?? [];
    const box = canvas.getBoundingClientRect();
    const column = Math.floor((event.clientX - box.left) / grid.step);
    const row = Math.floor((event.clientY - box.top) / grid.step);
    if (column >= grid.perRow || column < 0 || row < 0) return null;
    const first = (row * grid.perRow + column) * grid.per;
    for (let k = first; k < Math.min(slots.length, first + grid.per); k++) if (slots[k] !== null) return k;
    return first < slots.length ? first : null;
  };
  canvas.addEventListener('click', (event) => {
    const slot = slotAt(event);
    if (slot !== null && segment.slots[slot] !== null) openActor(segment.actors.find((a) => a.slot === slot), true);
  });
  canvas.addEventListener('pointermove', (event) => {
    const slot = slotAt(event);
    if (slot === null) { hideTip(); return; }
    const actor = segment.actors.find((a) => a.slot === slot);
    showTip({ left: event.clientX - 1, width: 2, bottom: event.clientY + 8 }, actor ? [`slot ${slot}`, `session ${actor.session} · ${stateName(actor.state)}`] : [`slot ${slot}`, 'free']);
  });
  canvas.addEventListener('pointerleave', hideTip);
  canvas.addEventListener('keydown', (event) => {
    const live = segment?.actors ?? [];
    if (!live.length) return;
    const selected = selectedSlot() ?? live[0].slot;
    const at = Math.max(0, live.findIndex((a) => a.slot === selected));
    const down = Math.max(1, grid.perRow * grid.per);
    const moves = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: down, ArrowUp: -down, Home: -Infinity, End: Infinity };
    if (!(event.key in moves)) return;
    event.preventDefault();
    const step = moves[event.key];
    let next;
    if (Math.abs(step) === 1) next = live[Math.min(live.length - 1, Math.max(0, at + step))];
    else if (step === -Infinity) next = live[0];
    else if (step === Infinity) next = live[live.length - 1];
    else next = step > 0 ? live.find((a) => a.slot >= selected + step) ?? live[live.length - 1] : [...live].reverse().find((a) => a.slot <= selected + step) ?? live[0];
    openActor(next, false);
  });

  // ---- the inspector: a record of any pool, its links followed
  function openRecord(target, { reset = false } = {}) {
    history = reset ? [target] : [target, ...history.filter((t) => !(t.pool === target.pool && t.index === target.index))].slice(0, 32);
    if (target.pool === 'actors') {
      const i = shown.findIndex((a) => a.slot === target.index);
      if (i >= 0) cursor = i;
    }
    renderRecord();
    list.draw();
    grid.draw();
    updateStatus();
  }
  function goBack() {
    if (history.length < 2) return false;
    history = history.slice(1);
    renderRecord();
    list.draw();
    grid.draw();
    updateStatus();
    return true;
  }
  back.addEventListener('click', goBack);

  // A record's rows: decode.mjs's own decodeRecord when it has one (with
  // resolved text and links), else the layout's fields read here.
  function recordOf(target) {
    const pool = segment?.pools.find((p) => p.name === target.pool);
    if (!pool || pool.offset === null) return null;
    if (typeof decode.decodeRecord === 'function') {
      try {
        const offset = pool.offset + target.index * pool.stride;
        const record = decode.decodeRecord(pack, segment, pool.element, offset);
        if (record && (record.rows ?? record.fields)?.length) return { pool, offset, rows: record.rows ?? record.fields, strings: record.strings };
      } catch { /* fall back to the layout's fields */ }
    }
    if (target.pool === 'actors') return { pool, ...actorRecord(pack.layout, segment.region, pool, target.index) };
    const view = new DataView(segment.region.buffer, segment.region.byteOffset, segment.region.byteLength);
    const offset = pool.offset + target.index * pool.stride;
    return { pool, offset, rows: fieldRows(pack.layout, view, offset, pool.element) };
  }
  // Where a row points: the decoder's link, or an index or handle field's pool.
  function linkOf(row) {
    if (row.link) {
      if (typeof row.link === 'string') { const m = /^([a-z_]+)\[(\d+)\]$/.exec(row.link); return m ? { pool: m[1], index: Number(m[2]) } : null; }
      const index = row.link.index ?? row.link.slot;
      if (row.link.pool && typeof index === 'number') return { pool: row.link.pool, index };
    }
    const pool = /^(index|handle):(.+)$/.exec(row.kind ?? '');
    if (!pool || typeof row.value !== 'number') return undefined;
    const index = pool[1] === 'handle' ? handleSlot(row.value) : row.value === pack.layout.nil ? null : row.value;
    return index === null ? null : { pool: pool[2], index };
  }
  const titleOf = (target) => {
    if (target.pool !== 'actors') return `${target.pool}[${target.index}]`;
    const actor = segment?.actors.find((a) => a.slot === target.index);
    return actor ? `Actor ${actor.session}${labelOf(actor) ? ` · ${labelOf(actor)}` : ''}` : `actors[${target.index}]`;
  };
  function linkButton(target, text) {
    const button = el('button', { type: 'button', class: 'insp__link' }, text ?? `${target.pool}[${target.index}]`);
    button.addEventListener('click', () => openRecord(target));
    return button;
  }
  function renderRecord() {
    back.disabled = history.length < 2;
    crumbs.replaceChildren(...[...history].reverse().flatMap((target, i, all) => {
      const last = i === all.length - 1;
      const node = last ? el('span', { class: 'insp__crumb is-current', 'aria-current': 'location' }, titleOf(target))
        : (() => { const b = el('button', { type: 'button', class: 'insp__crumb' }, titleOf(target)); b.addEventListener('click', () => { history = history.slice(all.length - 1 - i); renderRecord(); list.draw(); grid.draw(); updateStatus(); }); return b; })();
      return last ? [node] : [node, el('span', { class: 'insp__sep', 'aria-hidden': 'true' }, '›')];
    }));
    const target = history[0];
    if (!target) { recordBody.replaceChildren(el('p', { class: 'insp__empty' }, segment ? 'Pick an actor in the list or on the grid.' : 'Open a snapshot, or load the sample.')); return; }
    const record = recordOf(target);
    if (!record) { recordBody.replaceChildren(el('p', { class: 'insp__empty' }, `The ${target.pool} pool has no records in this segment.`)); return; }
    const { pool, offset, rows } = record;
    const actor = target.pool === 'actors' ? segment.actors.find((a) => a.slot === target.index) : null;
    const valueCell = (row) => {
      const link = linkOf(row);
      const raw = rawText(row);
      if (link === null) return el('td', {}, el('span', { class: 'insp__muted' }, row.kind?.startsWith('handle') ? 'none' : 'nil'), el('span', { class: 'insp__raw' }, raw));
      if (link) return el('td', {}, linkButton(link, row.text ?? (link.pool === 'actors' ? titleOf(link) : `${link.pool}[${link.index}]`)), el('span', { class: 'insp__raw' }, raw));
      if ((row.kind === 'string' || row.kind === 'event-name') && row.value === pack.layout.nil) return el('td', {}, el('span', { class: 'insp__muted' }, 'none'), el('span', { class: 'insp__raw' }, raw));
      if (row.text != null && row.text !== '') return el('td', {}, el('span', { class: 'insp__text' }, String(row.text)), el('span', { class: 'insp__raw' }, raw));
      if (row.name === 'state' && typeof row.value === 'number') return el('td', {}, el('span', { class: 'insp__text' }, stateName(row.value)), el('span', { class: 'insp__raw' }, raw));
      return el('td', {}, el('span', {}, raw));
    };
    const parts = [
      el('h2', { class: 'insp__record-title' }, titleOf(target)),
      el('p', { class: 'insp__muted' }, `${pool.name}[${target.index}] · ${pool.element} · offset ${number.format(offset)} · ${pool.stride} B`),
      el('table', { class: 'insp__fields' },
        el('thead', {}, el('tr', {}, ...['Offset', 'Field', 'Value'].map((h) => el('th', { scope: 'col' }, h)))),
        el('tbody', {}, ...rows.map((row) => el('tr', { title: `${row.type}${row.kind ? ` (${row.kind})` : ''}, ${row.size} bytes` },
          el('td', { class: 'insp__num' }, `+${row.offset}`), el('td', { class: 'insp__field' }, row.name), valueCell(row))))),
    ];
    if (actor?.mailbox?.length) {
      parts.push(el('h2', {}, `Mailbox · ${actor.mailbox.length}`),
        el('ol', { class: 'insp__chain' }, ...actor.mailbox.map((index, i) => el('li', {}, el('span', { class: 'insp__muted' }, `${i + 1}.`), linkButton({ pool: 'events', index }, eventLabel(index))))));
    }
    if (actor && actor.parent !== null && actor.parent !== undefined) {
      parts.push(el('h2', {}, 'Parent'), el('p', {}, linkButton({ pool: 'actors', index: actor.parent }, titleOf({ pool: 'actors', index: actor.parent }))));
    }
    recordBody.replaceChildren(...parts);
  }
  function eventLabel(index) {
    const record = recordOf({ pool: 'events', index });
    const name = record?.rows?.find((row) => /(^|\.)name$/.test(row.name) && row.text)?.text ?? record?.strings?.name;
    return name ? `events[${index}] · ${name}` : `events[${index}]`;
  }

  // ---- the status bar
  function updateStatus() {
    if (!segment) { status.replaceChildren(el('div', { class: 'insp__status-in' }, pack ? 'No live segment' : 'No snapshot open')); return; }
    const target = history[0];
    const pool = target && segment.pools.find((p) => p.name === target.pool);
    const parts = [`segment ${segment.index}`, `${number.format(segment.actors.length)} actors`, `${number.format(segment.slots.length)} slots`];
    if (filter.value.trim()) parts.push(`${number.format(shown.length)} shown`);
    if (target && pool) parts.push(`${target.pool}[${target.index}] at ${number.format(pool.offset + target.index * pool.stride)}–${number.format(pool.offset + (target.index + 1) * pool.stride - 1)}`);
    status.replaceChildren(el('div', { class: 'insp__status-in' }, ...parts.map((text) => el('span', {}, text)),
      el('span', { class: 'insp__keys' }, el('kbd', {}, '/'), ' filter ', el('kbd', {}, 'j'), el('kbd', {}, 'k'), ' move ', el('kbd', {}, 'Esc'), ' back')));
  }

  // ---- the keyboard
  app.tabIndex = -1;
  document.addEventListener('keydown', (event) => {
    if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.altKey) return;
    const typing = event.target instanceof HTMLElement && (event.target.matches('input, textarea, select, [contenteditable]'));
    if (event.key === 'Escape') {
      if (event.target === filter && filter.value) { filter.value = ''; applyFilter(); event.preventDefault(); return; }
      if (event.target === filter) { filter.blur(); listScroll.focus(); return; }
      if (goBack()) { event.preventDefault(); return; }
      if (matchMedia(PHONE).matches && app.dataset.view === 'record') show('actors');
      return;
    }
    if (typing) {
      if (event.target === filter && (event.key === 'ArrowDown' || event.key === 'Enter')) { event.preventDefault(); listScroll.focus(); if (cursor < 0) moveCursor(1); }
      return;
    }
    if (!pack) return;
    if (event.key === '/') { event.preventDefault(); if (matchMedia(PHONE).matches) show('actors'); filter.focus(); filter.select(); }
    else if (event.key === 'j' || (event.key === 'ArrowDown' && event.target === listScroll)) { event.preventDefault(); moveCursor(1); }
    else if (event.key === 'k' || (event.key === 'ArrowUp' && event.target === listScroll)) { event.preventDefault(); moveCursor(-1); }
    else if (event.key === 'Enter' && event.target === listScroll && shown[cursor]) { event.preventDefault(); openActor(shown[cursor], true); recordBody.querySelector('button')?.focus(); }
  });

  new ResizeObserver(() => { grid.draw(); list.draw(); }).observe(panes);
  matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => requestAnimationFrame(() => grid.draw()));
  new MutationObserver(() => grid.draw()).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  renderRecord();
  updateStatus();
  await loadSample();
}

function rawText(row) {
  const value = row.value;
  if (Array.isArray(value)) return value.join(' ');
  if (typeof value === 'bigint') return value.toString();
  if (value && typeof value === 'object') return `${value.bytes ?? '?'} bytes`;
  return String(value);
}

// ---- helpers
function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes ?? {})) if (value !== null && value !== undefined) node.setAttribute(key, value);
  for (const child of children.flat()) if (child !== null && child !== undefined && child !== false && child !== '') node.append(child);
  return node;
}

function injectStyle() {
  if (document.querySelector('#inspector-style')) return;
  const style = el('style', { id: 'inspector-style' });
  style.textContent = `
.insp { --inset-start: var(--app-inset-start, 0px); --inset-end: var(--app-inset-end, 0px); --inset-bottom: var(--app-inset-bottom, 0px);
  position: relative; display: flex; flex-direction: column; flex: 1; min-height: 0; background: var(--bg-app); color: var(--fg-1); font: 13px var(--font-sans, system-ui); }
.insp.is-drop { outline: 2px dashed var(--accent); outline-offset: -4px; }
.insp h2 { margin: 0 0 6px; font: 500 11px/1.4 var(--font-mono, monospace); letter-spacing: .1em; text-transform: uppercase; color: var(--fg-3); }
.insp button:focus-visible, .insp [tabindex]:focus-visible, .insp__open:focus-within { outline: 2px solid var(--focus-ring); outline-offset: 1px; }
.insp kbd { display: inline-block; min-width: 16px; padding: 0 4px; margin: 0 1px; border: 1px solid var(--border-2); border-radius: 2px; font: 11px/15px var(--font-mono, monospace); text-align: center; color: var(--fg-2); background: var(--surface-1); }

.insp__bar { flex: none; display: flex; align-items: center; gap: 12px; min-height: 44px; padding: 6px 12px; border-bottom: 1px solid var(--border-1); background: var(--surface-1); }
.insp__title { display: flex; align-items: baseline; gap: 14px; min-width: 0; flex: 1; overflow: hidden; }
.insp__file { flex: none; font: 600 13px var(--font-mono, monospace); max-width: 28ch; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.insp__facts { display: flex; gap: 12px; margin: 0; min-width: 0; overflow: hidden; font: 12px var(--font-mono, monospace); white-space: nowrap; }
.insp__facts dt { color: var(--fg-4); }
.insp__facts dd { margin: 0 0 0 -8px; color: var(--fg-2); }
.insp__actions { flex: none; display: flex; align-items: center; gap: 8px; }
.insp__picker { position: absolute; width: 1px; height: 1px; opacity: 0; overflow: hidden; }
.insp__open { cursor: pointer; }
.insp__note { color: var(--fg-4); font-size: 12px; margin-right: 4px; white-space: nowrap; }

.insp__switch { display: none; }
.insp__message { flex: none; margin: 16px; padding: 12px 16px; border: 1px solid var(--state-waiting); background: var(--state-waiting-bg); border-radius: 2px; }
.insp__message[hidden] { display: none; }
.insp__message p { margin: 6px 0 0; color: var(--fg-2); }

.insp__panes { flex: 1; min-height: 0; display: grid; grid-template-columns: minmax(270px, 310px) minmax(0, 1fr) minmax(300px, 380px); }
.insp__panes[hidden] { display: none; }
.insp__pane { min-width: 0; min-height: 0; overflow: auto; overscroll-behavior: contain; }
.insp__nav { display: flex; flex-direction: column; overflow: hidden; border-right: 1px solid var(--border-1); background: var(--surface-1); }
.insp__group { padding: 10px 12px; border-bottom: 1px solid var(--border-1); }
.insp__group--grow { flex: 1; min-height: 0; display: flex; flex-direction: column; border-bottom: 0; padding-bottom: 0; }
.insp__count { color: var(--fg-2); letter-spacing: 0; }
.insp__segments { list-style: none; margin: 0; padding: 0; display: grid; gap: 4px; }
.insp__segment { display: flex; justify-content: space-between; gap: 8px; width: 100%; text-align: left; padding: 4px 8px; border: 1px solid var(--border-1); border-radius: 2px; background: transparent; color: var(--fg-1); font: 12px var(--font-mono, monospace); cursor: pointer; }
.insp__segment:hover { background: var(--surface-2); }
.insp__segment[aria-current="true"] { background: var(--surface-3); border-color: var(--border-strong); }
.insp__segment:disabled { opacity: .45; cursor: not-allowed; }
.insp__muted { color: var(--fg-3); font-size: 12px; }
.insp__pools { display: grid; gap: 3px; }
.insp__pool { display: grid; grid-template-columns: 76px minmax(0, 1fr) 56px; align-items: center; gap: 6px; font: 11px var(--font-mono, monospace); }
.insp__pool-name { color: var(--fg-2); overflow: hidden; text-overflow: ellipsis; }
.insp__pool-count { color: var(--fg-3); text-align: right; font-variant-numeric: tabular-nums; }
.insp__meter { position: relative; height: 8px; background: var(--bg-sunken); border: 1px solid var(--border-1); }
.insp__meter-high { position: absolute; inset: 0 auto 0 0; background: var(--accent-subtle); }
.insp__meter-used { position: absolute; inset: 0 auto 0 0; background: var(--accent); }
.insp__filter { width: 100%; height: 28px; margin-bottom: 6px; padding: 0 8px; border: 1px solid var(--border-2); border-radius: 2px; background: var(--bg-app); color: var(--fg-1); font: 12px var(--font-mono, monospace); }
.insp__filter:focus { outline: 2px solid var(--focus-ring); outline-offset: -1px; }
.insp__filter::placeholder { color: var(--fg-4); }
.insp__list-head, .insp__row { display: grid; grid-template-columns: minmax(0, 1.7fr) minmax(0, 1fr) 46px 46px; gap: 6px; align-items: center; padding: 0 6px; }
.insp__list-head { font: 500 10px var(--font-mono, monospace); letter-spacing: .08em; text-transform: uppercase; color: var(--fg-4); padding-bottom: 4px; border-bottom: 1px solid var(--border-1); }
.insp__list-head span:nth-child(n+3) { text-align: right; }
.insp__list { position: relative; flex: 1; min-height: 0; overflow: auto; overscroll-behavior: contain; margin: 0 -12px; padding: 0 12px; outline-offset: -2px; }
.insp__list-spacer { position: relative; margin-bottom: calc(8px + var(--inset-bottom)); }
.insp__row { position: absolute; left: 0; right: 0; height: 28px; border-radius: 2px; cursor: pointer; font: 12px var(--font-mono, monospace); color: var(--fg-2); }
.insp__row:hover { background: var(--surface-2); }
.insp__row.is-cursor { box-shadow: inset 2px 0 0 var(--accent); }
.insp__row.is-selected { background: var(--surface-3); color: var(--fg-1); }
.insp__session { display: flex; gap: 6px; min-width: 0; white-space: nowrap; }
.insp__label { color: var(--fg-3); overflow: hidden; text-overflow: ellipsis; }
.insp__state { display: flex; align-items: center; gap: 6px; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.insp__state i, .insp__legend i { flex: none; display: inline-block; width: 9px; height: 9px; border-radius: 1px; background: var(--dot); }
.insp__legend i.is-free { background: transparent; box-shadow: inset 0 0 0 1px var(--dot); }
.insp__num { text-align: right; font-variant-numeric: tabular-nums; color: var(--fg-3); white-space: nowrap; }
.insp__empty { color: var(--fg-3); font-size: 12px; padding: 12px 0; margin: 0; }

.insp__memory { display: flex; flex-direction: column; padding: 12px 16px 0; overflow: hidden; }
.insp__map { flex: none; display: flex; height: 22px; gap: 1px; margin-bottom: 14px; border: 1px solid var(--border-1); background: var(--border-1); }
.insp__band { min-width: 3px; flex-basis: 0; background: var(--surface-2); cursor: default; }
.insp__band--header { background: var(--fg-3); }
.insp__band--pool { background: var(--accent-subtle); box-shadow: inset 0 0 0 1px var(--accent); cursor: pointer; }
.insp__band--generations { background: var(--data-1); }
.insp__band--array { background: var(--data-2); }
.insp__band--gap { background: repeating-linear-gradient(135deg, var(--surface-1) 0 3px, var(--border-2) 3px 4px); }
.insp__band:hover, .insp__band:focus-visible { filter: brightness(.85); }
.insp__legend { flex: none; display: flex; flex-wrap: wrap; gap: 4px 14px; margin: 0 0 10px; font-size: 12px; color: var(--fg-2); }
.insp__legend span { display: inline-flex; align-items: center; gap: 6px; }
.insp__grid-scroll { flex: none; max-height: 45%; overflow: auto; overscroll-behavior: contain; margin-bottom: 16px; }
.insp__bands { flex: 1; min-height: 80px; overflow: auto; overscroll-behavior: contain; padding-bottom: 12px; }
.insp__bands td:nth-child(2) { width: 100%; }
.insp__grid { display: block; cursor: pointer; }

.insp__inspector { border-left: 1px solid var(--border-1); background: var(--surface-1); padding: 0 0 calc(16px + var(--inset-bottom)); }
.insp__crumbbar { position: sticky; top: 0; z-index: 1; display: flex; align-items: center; gap: 6px; min-height: 36px; padding: 4px 8px; border-bottom: 1px solid var(--border-1); background: var(--surface-1); }
.insp__back { flex: none; }
.insp__crumbs { display: flex; align-items: center; gap: 2px; min-width: 0; overflow-x: auto; white-space: nowrap; font: 12px var(--font-mono, monospace); scrollbar-width: none; }
.insp__crumb { padding: 2px 4px; border: 0; background: none; color: var(--accent-fg); font: inherit; cursor: pointer; }
.insp__crumb.is-current { color: var(--fg-1); cursor: default; }
.insp__sep { color: var(--fg-4); }
.insp__record-body { padding: 12px; }
.insp__record-body h2 { margin-top: 14px; }
.insp__record-body > .insp__record-title { margin: 0 0 2px; font: 600 15px var(--font-mono, monospace); letter-spacing: 0; text-transform: none; color: var(--fg-1); }
.insp__record-body > p { margin: 0 0 10px; }
.insp__fields { width: 100%; border-collapse: collapse; font: 12px var(--font-mono, monospace); }
.insp__fields th { text-align: left; font: 500 10px var(--font-mono, monospace); letter-spacing: .08em; text-transform: uppercase; color: var(--fg-4); padding: 4px 6px; border-bottom: 1px solid var(--border-2); }
.insp__fields td { padding: 3px 6px; border-bottom: 1px solid var(--border-1); vertical-align: baseline; }
.insp__fields td:first-child, .insp__fields td.insp__num { width: 1%; white-space: nowrap; }
.insp__field { color: var(--fg-2); overflow-wrap: anywhere; }
.insp__fields td:last-child { overflow-wrap: anywhere; }
.insp__text { color: var(--fg-1); }
.insp__raw { margin-left: 8px; color: var(--fg-4); font-size: 11px; }
.insp__fields td:last-child > span:only-child.insp__raw { margin-left: 0; }
.insp__link { padding: 0; border: 0; background: none; color: var(--accent-fg); font: inherit; cursor: pointer; text-decoration: underline; text-decoration-color: var(--border-2); text-underline-offset: 2px; text-align: left; }
.insp__chain { list-style: none; margin: 0; padding: 0; display: grid; gap: 2px; font: 12px var(--font-mono, monospace); }
.insp__chain li { display: flex; gap: 8px; }

.insp__status { flex: none; display: flex; align-items: center; gap: 16px; height: 28px; padding: 0 calc(12px + var(--inset-end)) 0 calc(12px + var(--inset-start)); border-top: 1px solid var(--border-1); background: var(--surface-1);
  font: 11px var(--font-mono, monospace); color: var(--fg-3); white-space: nowrap; }
.insp__status-in { display: flex; align-items: center; gap: inherit; flex: 1; min-width: 0; overflow: hidden; }
.insp__keys { margin-left: auto; color: var(--fg-4); }

.insp__tip { position: absolute; z-index: 10; display: grid; gap: 2px; max-width: 280px; padding: 6px 8px; background: var(--surface-1); border: 1px solid var(--border-2);
  border-radius: 2px; box-shadow: var(--shadow-pop, 0 4px 12px rgba(0,0,0,.15)); font: 12px var(--font-mono, monospace); pointer-events: none; }
.insp__tip[hidden] { display: none; }
.insp__tip span { color: var(--fg-2); }

@media (max-width: 1100px) {
  .insp__panes { grid-template-columns: minmax(230px, 260px) minmax(0, 1fr) minmax(260px, 320px); }
  .insp__note, .insp__facts dt:nth-of-type(n+3), .insp__facts dd:nth-of-type(n+3) { display: none; }
}
@media (max-width: 720px) {
  .insp__bar { flex-wrap: wrap; gap: 6px 10px; padding: 8px 12px; }
  .insp__title { flex-basis: 100%; }
  .insp__facts dt:nth-of-type(n+2), .insp__facts dd:nth-of-type(n+2) { display: none; }
  .insp__actions { flex: 1; }
  .insp__actions .ta-btn { flex: 1; justify-content: center; }
  .insp__switch { flex: none; display: grid; grid-template-columns: repeat(3, 1fr); gap: 2px; margin: 8px 12px; padding: 2px; border: 1px solid var(--border-1); border-radius: 2px; background: var(--bg-sunken); }
  .insp__switch button { height: 30px; border: 0; border-radius: 2px; background: transparent; color: var(--fg-2); font: 500 13px var(--font-sans, system-ui); cursor: pointer; }
  .insp__switch button[aria-selected="true"] { background: var(--surface-1); color: var(--fg-1); box-shadow: 0 0 0 1px var(--border-2); }
  .insp__panes { grid-template-columns: minmax(0, 1fr); border-top: 1px solid var(--border-1); }
  .insp__pane { display: none; border: 0; }
  .insp[data-view="actors"] .insp__nav, .insp[data-view="memory"] .insp__memory, .insp[data-view="record"] .insp__inspector { display: flex; flex-direction: column; }
  .insp[data-view="record"] .insp__inspector { display: block; }
  .insp__memory { padding: 12px; }
  .insp__keys { display: none; }
  .insp__status { gap: 10px; }
}`;
  document.head.append(style);
}
