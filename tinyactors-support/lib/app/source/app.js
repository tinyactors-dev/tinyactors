// The issue list: an MCP App (the io.modelcontextprotocol/ui extension,
// protocol 2026-01-26). The host renders it for known_issues and get_issue;
// it talks to the host in JSON-RPC over postMessage, and every operation is
// a call of the server's own tools (tools/call through the host), as the
// signed-in user. What it may offer, it learns from whoami and
// list_projects; the server refuses whatever it should not do anyway.
//
// Its interaction follows an issue tracker's habits (Linear's): issues
// grouped by state, a filter menu with removable chips, a search, display
// options (group, order), a side peek with the issue's properties, the
// keyboard (j/k, Enter, Esc, x, c, /, f, ?), multi-select with bulk
// actions, optimistic changes undone on a refusal, and a toast with each
// tool's one line. It reads every issue once (known_issues, state all) and
// filters and orders them here; Refresh reads them again.
(() => {
  'use strict';
  const PROTOCOL = '2026-01-26';
  const STATES = ['regressed', 'new', 'triaged', 'fixed', 'closed', 'wontfix'];   // the groups' order
  const OPEN = ['new', 'triaged', 'regressed'];
  const KINDS = ['docs', 'error', 'tooling', 'api', 'environment'];
  const STATE_NAMES = { new: 'New', triaged: 'Triaged', regressed: 'Regressed', fixed: 'Fixed', closed: 'Closed', wontfix: 'Declined' };
  const GROUPS = { state: 'State', component: 'Component', kind: 'Kind', none: 'No grouping' };
  const ORDERS = { cost: 'Cost (reports × minutes)', reports: 'Reports', newest: 'Newest', updated: 'Recently updated', title: 'Title' };
  const WIDE = 880;   // px: the peek sits beside the list from here, over it below
  const PHONE = 600;  // px: below it, the phone layout: one compact header row, two-line rows, the peek a full-width sheet

  // --- the host: JSON-RPC over postMessage ---------------------------------
  const pending = new Map();
  let nextID = 1;
  const send = (message) => window.parent.postMessage({ jsonrpc: '2.0', ...message }, '*');
  const request = (method, params) => new Promise((resolve, reject) => {
    const id = nextID++;
    pending.set(id, { resolve, reject });
    send({ id, method, params });
  });
  const notify = (method, params) => send({ method, params });
  const handlers = {};
  window.addEventListener('message', (event) => {
    if (event.source !== window.parent) return;
    const m = event.data;
    if (!m || m.jsonrpc !== '2.0') return;
    if (m.id !== undefined && !m.method) {
      const waiter = pending.get(m.id);
      pending.delete(m.id);
      if (!waiter) return;
      if (m.error) waiter.reject(new Error(m.error.message ?? 'refused by the host'));
      else waiter.resolve(m.result);
      return;
    }
    if (m.method === 'ping' || m.method === 'ui/resource-teardown') { send({ id: m.id, result: {} }); return; }
    handlers[m.method]?.(m.params ?? {});
    if (m.id !== undefined) send({ id: m.id, result: {} });
  });

  async function call(name, args = {}) {
    try {
      const result = await request('tools/call', { name, arguments: args });
      const data = result?.structuredContent ?? {};
      const text = result?.content?.find((c) => c.type === 'text')?.text ?? '';
      return { data, text, refused: !!result?.isError || !!data.error || ['refused', 'failed'].includes(data.answer) };
    } catch (error) {
      return { data: { error: error.message }, text: error.message, refused: true };
    }
  }

  // --- what the view knows ----------------------------------------------------
  const S = {
    projects: [], me: null, issues: [], loading: true, error: null,
    filters: { state: new Set(OPEN), component: new Set(), kind: new Set(), repo: new Set(), workaround: false, duplicate: false },
    search: '', group: 'state', order: 'cost', collapsed: new Set(),
    selected: new Set(), cursor: null, anchor: null,
    peek: null, detail: null, panel: null,   // panel: null | 'report' | 'decline' | 'delete'
    menu: null, context: {}, bulk: null, searching: false,
  };
  const short = (id) => String(id).replace(/^tinyactors:/, '');
  const byID = (id) => S.issues.find((i) => i.id === id);
  const project = (repo) => S.projects.find((p) => p.repo === repo || p.slug === repo);
  const maintains = (repo) => !!S.me && !!project(repo) && S.me.maintainerOf.includes(project(repo).repoID);

  // --- the host's context: theme, size, display mode -----------------------
  function applyContext(context = {}) {
    S.context = { ...S.context, ...context };
    const theme = S.context.theme;
    if (theme === 'dark' || theme === 'light') document.documentElement.dataset.theme = theme;
    document.documentElement.dataset.mode = S.context.displayMode ?? 'inline';
    const dimensions = S.context.containerDimensions ?? {};
    const root = document.documentElement.style;
    root.maxHeight = dimensions.maxHeight ? `${dimensions.maxHeight}px` : '';
    // a width bound the host gives is the view's too, whatever width the frame has
    root.maxWidth = dimensions.maxWidth ? `${dimensions.maxWidth}px` : dimensions.width ? `${dimensions.width}px` : '';
    // touch: the host's word, else the browser's (a coarse pointer); every control is then a 44 px target
    const touch = S.context.deviceCapabilities?.touch ?? window.matchMedia?.('(pointer: coarse)').matches ?? false;
    document.documentElement.dataset.touch = String(!!touch);
    // the host's safe area (a phone's notch, its own floating controls), as CSS variables
    const inset = S.context.safeAreaInsets ?? {};
    for (const side of ['top', 'right', 'bottom', 'left']) root.setProperty(`--safe-${side}`, `${Number(inset[side]) || 0}px`);
  }
  const canExpand = () => (S.context.availableDisplayModes ?? []).includes('fullscreen');
  handlers['ui/notifications/host-context-changed'] = (params) => { applyContext(params); render(); };
  handlers['ui/notifications/tool-input'] = ({ arguments: args = {} }) => {
    if (args.repo) S.filters.repo = new Set([project(args.repo)?.repo ?? args.repo]);
    if (args.component) S.filters.component = new Set([args.component]);
    if (args.state) S.filters.state = args.state === 'all' ? new Set() : args.state === 'open' ? new Set(OPEN) : new Set([args.state]);
    if (args.id) S.peek = args.id;
  };
  handlers['ui/notifications/tool-result'] = (result) => {
    const data = result?.structuredContent ?? {};
    if (Array.isArray(data.issues) && S.loading) { S.issues = data.issues; S.loading = false; }
    else if (data.answer === 'issue') { S.peek = data.id; S.detail = data; }
    render();
  };

  // --- reading -------------------------------------------------------------------
  async function load() {
    S.loading = S.issues.length === 0; S.error = null; render();
    const { data, text, refused } = await call('known_issues', { state: 'all' });
    S.loading = false;
    if (refused) { S.error = text || 'The issues could not be read.'; render(); return; }
    S.issues = data.issues ?? [];
    if (data.sinceYourLastVisit?.length) toast(text);
    for (const id of [...S.selected]) if (!byID(id)) S.selected.delete(id);
    render();
    if (S.peek) openPeek(S.peek, { quiet: true });
  }
  async function openPeek(id, { quiet = false } = {}) {
    S.peek = id; S.cursor = id;
    if (!quiet) S.panel = null;
    if (S.detail?.id !== id) S.detail = null;
    render();
    const { data, text, refused } = await call('get_issue', { id });
    if (S.peek !== id) return;
    if (refused || data.answer !== 'issue') { toast(text, 'error'); S.detail = { id, missing: text }; render(); return; }
    S.detail = data;
    merge(data);
    render();
  }
  // what get_issue says, into the list's row of the issue
  function merge(d) {
    const row = byID(d.id);
    if (!row) return;
    Object.assign(row, { title: d.title, state: d.state, kind: d.kind, count: d.count, minutes: d.minutes,
      workaround: d.workaround ? d.workaround.text : null, filed: d.filed ?? row.filed, updated: d.updated ?? row.updated,
      attachments: Array.isArray(d.attachments) ? d.attachments.length : d.attachments ?? row.attachments });
  }

  // --- changing: optimistic, undone on a refusal, each outcome a toast ----------
  async function mutate(id, tool, args, patch, { quiet = false } = {}) {
    const row = byID(id), before = row ? { ...row } : null, index = S.issues.indexOf(row);
    if (row && patch === 'remove') S.issues.splice(index, 1);
    else if (row && patch) Object.assign(row, patch);
    if (patch === 'remove' && S.peek === id) closePeek();
    render();
    const { data, text, refused } = await call(tool, { id, ...args });
    if (refused) {
      if (before && patch === 'remove') S.issues.splice(index, 0, before);
      else if (row && before) Object.assign(row, before);
      if (!quiet) toast(text, 'error');
      render();
      return { ok: false, text };
    }
    if (!quiet) toast(text);
    if (patch !== 'remove' && S.peek === id) await openPeek(id, { quiet: true });
    else if (patch !== 'remove') { const fresh = await call('get_issue', { id }); if (!fresh.refused) { merge(fresh.data); render(); } }
    return { ok: true, text, data };
  }
  const actions = {
    triage: (i, fields = {}) => mutate(i.id, 'triage', { kind: fields.kind ?? i.kind, title: fields.title || undefined },
      { state: 'triaged', kind: fields.kind ?? i.kind, ...(fields.title ? { title: fields.title } : {}) }),
    close: (i) => mutate(i.id, 'close', {}, { state: 'closed' }),
    wontfix: (i, reason) => mutate(i.id, 'wontfix', { reason }, { state: 'wontfix' }),
    remove: (i, reason) => mutate(i.id, 'delete_issue', { reason: reason || undefined }, 'remove'),
  };
  // what a maintainer may do to an issue in its state, as the tools allow it
  const allowed = (i) => maintains(i.repo) ? {
    triage: i.state === 'new', close: i.state === 'fixed', wontfix: OPEN.includes(i.state), remove: true } : {};

  async function bulk(kind, reason) {
    const targets = [...S.selected].map(byID).filter((i) => i && allowed(i)[kind]);
    const skipped = S.selected.size - targets.length;
    const names = { triage: 'Triaging', close: 'Closing', wontfix: 'Declining' };
    const refusals = [];
    let done = 0;
    for (const i of targets) {
      S.bulk = `${names[kind]} ${done + 1} of ${targets.length}…`; render();
      const outcome = kind === 'wontfix' ? await mutate(i.id, 'wontfix', { reason }, { state: 'wontfix' }, { quiet: true })
        : kind === 'close' ? await mutate(i.id, 'close', {}, { state: 'closed' }, { quiet: true })
        : await mutate(i.id, 'triage', { kind: i.kind }, { state: 'triaged' }, { quiet: true });
      if (outcome.ok) done++; else refusals.push(`${short(i.id)}: ${outcome.text}`);
    }
    S.bulk = null; S.selected.clear(); S.panel = null;
    const past = { triage: 'Triaged', close: 'Closed', wontfix: 'Declined' }[kind];
    const lines = [`${past} ${done} of ${targets.length}.` + (skipped ? ` ${skipped} not in a state for it.` : ''), ...refusals];
    toast(lines.join('\n'), refusals.length ? 'error' : '');
    render();
  }

  // --- which issues show, and in what order --------------------------------------
  const cost = (i) => i.count * i.minutes;
  const time = (t) => (t ? Date.parse(t) : -Infinity);
  const ORDER = {
    cost: (a, b) => cost(b) - cost(a) || a.n - b.n,
    reports: (a, b) => b.count - a.count || cost(b) - cost(a),
    newest: (a, b) => time(b.filed) - time(a.filed) || b.n - a.n,
    updated: (a, b) => time(b.updated) - time(a.updated) || b.n - a.n,
    title: (a, b) => a.title.localeCompare(b.title),
  };
  function visible() {
    const f = S.filters, words = S.search.trim().toLowerCase().split(/\s+/).filter(Boolean);
    return S.issues.filter((i) => (!f.state.size || f.state.has(i.state)) && (!f.component.size || f.component.has(i.component))
      && (!f.kind.size || f.kind.has(i.kind)) && (!f.repo.size || f.repo.has(i.repo)) && (!f.workaround || !!i.workaround)
      && (!f.duplicate || !!i.possibleDuplicateOf)
      && words.every((w) => `${short(i.id)} ${i.title} ${i.excerpt ?? ''}`.toLowerCase().includes(w)))
      .sort(ORDER[S.order]);
  }
  function groups(list) {
    if (S.group === 'none') return [{ key: 'all', label: null, issues: list }];
    const keys = S.group === 'state' ? STATES : [...new Set(list.map((i) => i[S.group]))].sort();
    return keys.map((key) => ({ key, label: S.group === 'state' ? STATE_NAMES[key] : key,
      issues: list.filter((i) => i[S.group] === key) })).filter((g) => g.issues.length);
  }
  // the rows the keyboard walks: what is shown, collapsed groups left out
  const walkable = () => groups(visible()).filter((g) => !S.collapsed.has(`${S.group}:${g.key}`)).flatMap((g) => g.issues);

  // --- drawing ------------------------------------------------------------------
  function h(tag, attributes = {}, ...children) {
    const element = tag === 'svg' || tag === 'path' || tag === 'circle' || tag === 'line'
      ? document.createElementNS('http://www.w3.org/2000/svg', tag) : document.createElement(tag);
    for (const [key, value] of Object.entries(attributes)) {
      if (value === undefined || value === null || value === false) continue;
      if (key.startsWith('on')) element.addEventListener(key.slice(2), value);
      else if (key === 'dataset') Object.assign(element.dataset, value);
      else element.setAttribute(key, value === true ? '' : value);
    }
    for (const child of children.flat()) if (child !== null && child !== undefined && child !== false)
      element.append(child instanceof Node ? child : String(child));
    return element;
  }
  const button = (label, variant, onclick, extra = {}) =>
    h('button', { class: `btn btn--${variant}`, type: 'button', onclick, ...extra }, label);
  const kbd = (key) => h('kbd', { class: 'kbd' }, key);

  // A state's icon: a shape and a colour each, so neither alone carries it.
  function icon(state) {
    const svg = h('svg', { class: 'status', viewBox: '0 0 14 14', width: 14, height: 14, 'aria-hidden': 'true', dataset: { state } });
    const ring = (extra = {}) => h('circle', { cx: 7, cy: 7, r: 5.5, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.5, ...extra });
    if (state === 'new') svg.append(ring({ 'stroke-dasharray': '2.2 1.8' }));
    else if (state === 'triaged') svg.append(ring(), h('path', { d: 'M7 3.5 A3.5 3.5 0 0 1 7 10.5 Z', fill: 'currentColor' }));
    else if (state === 'regressed') svg.append(ring(), h('line', { x1: 7, y1: 4, x2: 7, y2: 7.8, stroke: 'currentColor', 'stroke-width': 1.6, 'stroke-linecap': 'round' }),
      h('circle', { cx: 7, cy: 10, r: 0.9, fill: 'currentColor' }));
    else if (state === 'fixed') svg.append(ring(), h('path', { d: 'M4.6 7.2 L6.3 8.9 L9.4 5.4', fill: 'none', stroke: 'currentColor', 'stroke-width': 1.5, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }));
    else if (state === 'closed') svg.append(h('circle', { cx: 7, cy: 7, r: 6.25, fill: 'currentColor' }),
      h('path', { d: 'M4.6 7.2 L6.3 8.9 L9.4 5.4', fill: 'none', stroke: 'var(--surface-1)', 'stroke-width': 1.6, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }));
    else svg.append(ring(), h('line', { x1: 3.4, y1: 10.6, x2: 10.6, y2: 3.4, stroke: 'currentColor', 'stroke-width': 1.5 }));
    return svg;
  }
  const stateLabel = (state) => h('span', { class: 'state-label', dataset: { state } }, icon(state), STATE_NAMES[state] ?? state);

  const app = document.getElementById('app');
  const regions = {};
  function render() {
    const wide = window.innerWidth >= WIDE;
    document.documentElement.dataset.layout = wide ? 'wide' : 'narrow';
    document.documentElement.dataset.size = window.innerWidth < PHONE ? 'phone' : 'regular';
    regions.toolbar ??= toolbar();
    refreshToolbar();
    const list = S.loading ? h('div', { class: 'state-box' }, h('p', {}, 'Reading the issues…'))
      : S.error ? h('div', { class: 'state-box', role: 'alert' }, h('p', {}, S.error), button('Try again', 'secondary', load))
      : listView();
    const peek = S.panel === 'report' ? reportForm() : S.peek ? peekView() : null;
    const body = h('div', { class: `body${peek ? ' has-peek' : ''}` },
      h('section', { class: 'pane pane--list', 'aria-label': 'Issues', hidden: !!(peek && !wide) }, chips(), bulkBar(), list),
      peek ? h('section', { class: 'pane pane--peek', 'aria-label': 'Issue' }, peek) : null);
    const focused = document.activeElement;
    const keep = focused && regions.toolbar.contains(focused) ? focused : null;
    const onRow = !!focused?.classList?.contains('row');
    regions.toasts ??= h('div', { class: 'toasts', role: 'status', 'aria-live': 'polite' });
    const menu = menuView();
    app.replaceChildren(...[regions.toolbar, regions.toasts, body, menu].filter(Boolean));
    // the chips' row scrolls sideways on a phone: a fade at its edge says so, when there is more
    const chipRow = app.querySelector('.chips');
    chipRow?.classList.toggle('scrolls', chipRow.scrollWidth > chipRow.clientWidth + 1);
    // a menu opens just under the header, however many rows the header has
    if (menu) menu.style.top = `${Math.round(regions.toolbar.offsetTop + regions.toolbar.offsetHeight + 4)}px`;
    for (const area of app.querySelectorAll('.title-edit')) fitHeight(area);
    if (keep) keep.focus();
    else if (onRow) focusCursor(false);
  }

  function toolbar() {
    const search = h('label', { class: 'ta-control search' },
      h('span', { class: 'search-glyph', 'aria-hidden': 'true' }, '⌕'),
      h('input', { id: 'search', type: 'search', placeholder: 'Search issues', 'aria-label': 'Search issues', autocomplete: 'off',
        oninput: (e) => { S.search = e.target.value; render(); },
        onkeydown: (e) => { if (e.key === 'Escape') { e.target.value = ''; S.search = ''; S.searching = false; e.target.blur(); render(); }
                            if (e.key === 'Enter' || e.key === 'ArrowDown') { e.preventDefault(); e.target.blur(); moveCursor(0); } } }),
      kbd('/'));
    // On a phone (data-size="phone") the header is one row: the count, a search button that opens the
    // search as a full-width row under it, Filter, a More menu (Display, Refresh, Full screen, Shortcuts)
    // and Report as "+"; the controls only a wide header has are .wide-only, the phone's .phone-only.
    return h('header', { class: 'toolbar' },
      h('span', { class: 'mark' }, h('span', { class: 'wide-only' }, 'tinyactors ', h('b', {}, '·'), ' '), 'issues'),
      h('span', { class: 'count', id: 'count' }),
      h('span', { class: 'spacer' }),
      search,
      button(glyph('search'), 'ghost', () => toggleSearch(), { id: 'search-toggle', class: 'btn btn--ghost btn--icon phone-only',
        'aria-label': 'Search', 'aria-expanded': 'false', 'aria-controls': 'search' }),
      button([glyph('filter'), h('span', { class: 'btn-label' }, 'Filter')], 'ghost', () => toggleMenu('filter'),
        { id: 'filter-button', 'aria-haspopup': 'true', 'aria-label': 'Filter', title: 'Filter (f)' }),
      button('Display', 'ghost', () => toggleMenu('display'), { id: 'display-button', class: 'btn btn--ghost wide-only', 'aria-haspopup': 'true', title: 'Group and order' }),
      button('↻', 'ghost', load, { class: 'btn btn--ghost btn--icon wide-only', 'aria-label': 'Refresh', title: 'Refresh' }),
      button('⤢', 'ghost', expand, { id: 'expand', class: 'btn btn--ghost btn--icon wide-only', 'aria-label': 'Expand', title: 'Full screen', hidden: true }),
      button('?', 'ghost', () => toggleMenu('help'), { class: 'btn btn--ghost btn--icon wide-only', 'aria-label': 'Keyboard shortcuts', title: 'Keyboard shortcuts (?)' }),
      button('⋯', 'ghost', () => toggleMenu('more'), { id: 'more-button', class: 'btn btn--ghost btn--icon phone-only', 'aria-haspopup': 'true',
        'aria-label': 'More', title: 'Display, refresh, full screen' }),
      button([h('span', { class: 'phone-only', 'aria-hidden': 'true' }, '+'), h('span', { class: 'btn-label' }, 'Report issue')], 'primary', () => openReport(),
        { id: 'report', hidden: true, 'aria-label': 'Report issue', title: 'Report an issue (c)' }));
  }
  // small line icons for the compact header (currentColor, so they take the button's colour in either theme)
  function glyph(name) {
    const svg = h('svg', { class: 'glyph', viewBox: '0 0 16 16', width: 16, height: 16, 'aria-hidden': 'true' });
    const stroke = { fill: 'none', stroke: 'currentColor', 'stroke-width': 1.5, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' };
    if (name === 'search') svg.append(h('circle', { cx: 7, cy: 7, r: 4.5, ...stroke }), h('line', { x1: 10.5, y1: 10.5, x2: 14, y2: 14, ...stroke }));
    else if (name === 'filter') svg.append(h('path', { d: 'M2.5 3.5 H13.5 L9.25 8.5 V12.5 L6.75 13.5 V8.5 Z', ...stroke }));
    return svg;
  }
  function toggleSearch(open = !S.searching) {
    S.searching = open;
    refreshToolbar();
    if (open) regions.toolbar.querySelector('#search').focus();
  }
  function refreshToolbar() {
    const shown = visible().length;
    regions.toolbar.querySelector('#count').textContent = S.loading ? '' : `${shown} of ${S.issues.length}`;
    regions.toolbar.querySelector('#report').hidden = !S.me;
    regions.toolbar.querySelector('#expand').hidden = !canExpand() || S.context.displayMode === 'fullscreen';
    regions.toolbar.querySelector('#filter-button').setAttribute('aria-expanded', String(S.menu === 'filter'));
    regions.toolbar.querySelector('#display-button').setAttribute('aria-expanded', String(S.menu === 'display'));
    regions.toolbar.querySelector('#more-button').setAttribute('aria-expanded', String(S.menu === 'more'));
    const searching = S.searching || !!S.search.trim();
    regions.toolbar.classList.toggle('searching', searching);
    regions.toolbar.querySelector('#search-toggle').setAttribute('aria-expanded', String(searching));
  }

  // --- filters: the menu and its chips -------------------------------------------
  const FIELDS = {
    state: { label: 'State', options: () => STATES.map((s) => [s, STATE_NAMES[s]]) },
    kind: { label: 'Kind', options: () => KINDS.map((k) => [k, k]) },
    // the components of the repositories shown: those a filter picked, else those with issues in the list
    // (every component of every repository made the menu scroll); a component already picked stays
    component: { label: 'Component', options: () => {
      const shown = S.filters.repo.size ? S.filters.repo : new Set(S.issues.map((i) => i.repo));
      return [...new Set([...S.issues.filter((i) => shown.has(i.repo)).map((i) => i.component),
        ...S.projects.filter((p) => shown.has(p.repo)).flatMap((p) => p.components), ...S.filters.component])]
        .sort().map((c) => [c, c]);
    } },
    repo: { label: 'Repository', options: () => S.projects.map((p) => [p.repo, p.repo]) },
  };
  function chips() {
    const f = S.filters, list = [];
    for (const [field, { label }] of Object.entries(FIELDS)) {
      if (!f[field].size) continue;
      const values = [...f[field]].map((v) => field === 'state' ? STATE_NAMES[v] : v);
      const text = field === 'state' && f.state.size === OPEN.length && OPEN.every((s) => f.state.has(s)) ? 'open' : values.join(', ');
      list.push(chip(`${label} is ${text}`, () => { f[field].clear(); render(); }));
    }
    if (f.workaround) list.push(chip('Has a workaround', () => { f.workaround = false; render(); }));
    if (f.duplicate) list.push(chip('Possible duplicate', () => { f.duplicate = false; render(); }));
    if (S.search.trim()) list.push(chip(`Matches “${S.search.trim()}”`, () => { S.search = ''; regions.toolbar.querySelector('#search').value = ''; render(); }));
    if (!list.length) return null;
    return h('div', { class: 'chips', role: 'group', 'aria-label': 'Active filters' }, list,
      list.length > 1 ? button('Clear', 'ghost', () => { for (const k of Object.keys(FIELDS)) f[k].clear(); f.workaround = f.duplicate = false;
        S.search = ''; regions.toolbar.querySelector('#search').value = ''; render(); }) : null);
  }
  const chip = (text, remove) => h('span', { class: 'chip' }, text,
    h('button', { type: 'button', class: 'chip-x', 'aria-label': `Remove filter: ${text}`, onclick: remove }, '×'));

  function toggleMenu(name) { S.menu = S.menu === name ? null : name; render(); if (S.menu) app.querySelector('.menu [tabindex="0"], .menu input, .menu button')?.focus(); }
  function menuView() {
    if (!S.menu) return null;
    const close = () => { S.menu = null; render(); };
    if (S.menu === 'filter') {
      const f = S.filters;
      const sections = Object.entries(FIELDS).map(([field, { label, options }]) => h('fieldset', { class: 'menu-section' },
        h('legend', {}, label), options().map(([value, text]) => h('label', { class: 'option' },
          h('input', { type: 'checkbox', checked: f[field].has(value), onchange: (e) => {
            if (e.target.checked) f[field].add(value); else f[field].delete(value); render(); } }),
          field === 'state' ? icon(value) : null, h('span', {}, text)))));
      const toggles = h('fieldset', { class: 'menu-section' }, h('legend', {}, 'Has'),
        h('label', { class: 'option' }, h('input', { type: 'checkbox', checked: f.workaround, onchange: (e) => { f.workaround = e.target.checked; render(); } }), h('span', {}, 'a workaround')),
        h('label', { class: 'option' }, h('input', { type: 'checkbox', checked: f.duplicate, onchange: (e) => { f.duplicate = e.target.checked; render(); } }), h('span', {}, 'a possible duplicate')));
      return h('div', { class: 'menu menu--filter', role: 'dialog', 'aria-label': 'Filter' }, sections, toggles,
        h('div', { class: 'menu-foot' }, button('Done', 'secondary', close)));
    }
    if (S.menu === 'display') {
      const radios = (name, options, value, set) => h('fieldset', { class: 'menu-section' }, h('legend', {}, name),
        Object.entries(options).map(([key, text]) => h('label', { class: 'option' },
          h('input', { type: 'radio', name, value: key, checked: key === value, onchange: () => { set(key); render(); } }), h('span', {}, text))));
      return h('div', { class: 'menu menu--display', role: 'dialog', 'aria-label': 'Display' },
        radios('Group by', GROUPS, S.group, (v) => { S.group = v; }), radios('Order by', ORDERS, S.order, (v) => { S.order = v; }),
        h('div', { class: 'menu-foot' }, button('Done', 'secondary', close)));
    }
    if (S.menu === 'more') {
      const item = (text, go, extra = {}) => h('button', { type: 'button', class: 'option option--button', onclick: go, ...extra }, text);
      return h('div', { class: 'menu menu--more', role: 'menu', 'aria-label': 'More' },
        item('Display…', () => toggleMenu('display'), { role: 'menuitem' }),
        item('Refresh', () => { S.menu = null; load(); }, { role: 'menuitem' }),
        canExpand() && S.context.displayMode !== 'fullscreen' ? item('Full screen', () => { S.menu = null; expand(); }, { role: 'menuitem' }) : null,
        S.context.deviceCapabilities?.hover === false ? null : item('Keyboard shortcuts', () => toggleMenu('help'), { role: 'menuitem' }));
    }
    if (S.menu === 'help') {
      const rows = [['j / ↓', 'next issue'], ['k / ↑', 'previous issue'], ['Enter / o', 'open in the side peek'], ['Esc', 'close the peek, a menu, the selection'],
        ['x', 'select or unselect'], ['⇧-click', 'select a range'], ['/', 'search'], ['f', 'filter'], ['c', 'report an issue'], ['?', 'these shortcuts']];
      return h('div', { class: 'menu menu--help', role: 'dialog', 'aria-label': 'Keyboard shortcuts' },
        h('h3', {}, 'Keyboard'), h('dl', { class: 'keys' }, rows.flatMap(([k, d]) => [h('dt', {}, k), h('dd', {}, d)])),
        h('div', { class: 'menu-foot' }, button('Close', 'secondary', close)));
    }
    return null;
  }

  // --- the list ------------------------------------------------------------------
  function listView() {
    if (S.issues.length === 0) return h('div', { class: 'state-box' }, h('p', {}, 'No issues yet.'),
      S.me ? button('Report the first', 'secondary', () => openReport()) : null);
    const list = visible();
    if (list.length === 0) return h('div', { class: 'state-box' }, h('p', {}, 'No issues match the filters.'));
    return h('div', { class: 'list', role: 'listbox', 'aria-label': 'Issues', 'aria-multiselectable': 'true' },
      groups(list).map((g) => {
        const key = `${S.group}:${g.key}`, collapsed = S.collapsed.has(key);
        return h('div', { class: 'group', role: 'group', 'aria-label': g.label ?? 'Issues' },
          g.label ? h('button', { type: 'button', class: 'group-head', 'aria-expanded': String(!collapsed),
            onclick: () => { if (collapsed) S.collapsed.delete(key); else S.collapsed.add(key); render(); } },
            h('span', { class: 'caret', 'aria-hidden': 'true' }, collapsed ? '▸' : '▾'),
            S.group === 'state' ? icon(g.key) : null, h('span', {}, g.label), h('span', { class: 'group-count' }, String(g.issues.length))) : null,
          collapsed ? null : g.issues.map(row));
      }));
  }
  function row(i) {
    const selected = S.selected.has(i.id), current = S.cursor === i.id, peeked = S.peek === i.id, can = allowed(i);
    const quick = [];
    if (can.triage) quick.push(button('Triage', 'ghost', (e) => { e.stopPropagation(); actions.triage(i); }, { 'aria-label': `Triage ${short(i.id)}` }));
    if (can.close) quick.push(button('Close', 'ghost', (e) => { e.stopPropagation(); actions.close(i); }, { 'aria-label': `Close ${short(i.id)}` }));
    if (can.wontfix) quick.push(button('Decline…', 'ghost', (e) => { e.stopPropagation(); openPeek(i.id).then(() => { S.panel = 'decline'; render(); }); },
      { 'aria-label': `Decline ${short(i.id)}` }));
    return h('div', { class: `row${selected ? ' is-selected' : ''}${peeked ? ' is-peeked' : ''}`, role: 'option', id: `row-${short(i.id)}`,
      'aria-selected': String(selected), tabindex: current ? '0' : '-1', dataset: { id: i.id }, title: i.title,
      onclick: (e) => rowClick(e, i), onfocus: () => { S.cursor = i.id; } },
      h('input', { type: 'checkbox', class: 'pick', checked: selected, tabindex: '-1', 'aria-label': `Select ${short(i.id)}`,
        onclick: (e) => { e.stopPropagation(); select(i.id, e.shiftKey ? 'range' : 'toggle'); } }),
      icon(i.state),
      h('span', { class: 'id' }, short(i.id)),
      h('span', { class: 'title' }, i.title),
      h('span', { class: 'labels' }, h('span', { class: 'tag' }, i.component), h('span', { class: 'tag tag--kind' }, i.kind)),
      quick.length ? h('span', { class: 'quick' }, quick) : null,
      h('span', { class: 'meta' },
        i.workaround ? h('span', { class: 'has-workaround', title: `Workaround: ${i.workaround}`, 'aria-label': 'has a workaround' }, '⚑') : null,
        i.attachments ? h('span', { class: 'files', title: `${i.attachments} files` }, `⎙${i.attachments}`) : null,
        i.possibleDuplicateOf ? h('span', { class: 'dupe', title: `May be ${i.possibleDuplicateOf.id}` }, '≈') : null,
        h('span', { class: 'cost', title: `${i.count} ${i.count === 1 ? 'report' : 'reports'}${i.minutes ? ` · ${i.minutes} min` : ''}` },
          i.minutes ? `${i.count}× · ${i.minutes}m` : `${i.count}×`)));
  }
  function rowClick(event, i) {
    if (event.shiftKey) { select(i.id, 'range'); return; }
    if (event.metaKey || event.ctrlKey) { select(i.id, 'toggle'); return; }
    openPeek(i.id);
  }
  function select(id, how) {
    if (how === 'range' && S.anchor) {
      const ids = walkable().map((i) => i.id), a = ids.indexOf(S.anchor), b = ids.indexOf(id);
      if (a >= 0 && b >= 0) for (const x of ids.slice(Math.min(a, b), Math.max(a, b) + 1)) S.selected.add(x);
    } else if (S.selected.has(id)) S.selected.delete(id);
    else S.selected.add(id);
    S.anchor = id; S.cursor = id;
    render();
  }
  function bulkBar() {
    if (!S.selected.size) return null;
    const targets = [...S.selected].map(byID).filter(Boolean);
    const count = (kind) => targets.filter((i) => allowed(i)[kind]).length;
    const label = (text, kind) => `${text}${count(kind) ? ` (${count(kind)})` : ''}`;
    const bar = h('div', { class: 'bulk', role: 'toolbar', 'aria-label': 'Selected issues' },
      h('span', { class: 'bulk-count' }, `${S.selected.size} selected`));
    if (S.bulk) bar.append(h('span', { class: 'bulk-progress' }, S.bulk));
    else if (targets.some((i) => maintains(i.repo))) {
      bar.append(button(label('Triage', 'triage'), 'secondary', () => bulk('triage'), { disabled: !count('triage') }),
        button(label('Close', 'close'), 'secondary', () => bulk('close'), { disabled: !count('close') }),
        button(label('Decline…', 'wontfix'), 'secondary', () => { S.panel = 'bulk-decline'; render(); }, { disabled: !count('wontfix') }));
      if (S.panel === 'bulk-decline') {
        const reason = h('input', { 'aria-label': 'Why they will not be fixed', placeholder: 'Why they will not be fixed' });
        bar.append(h('label', { class: 'ta-control grow' }, reason),
          button('Decline', 'danger', () => reason.value.trim() ? bulk('wontfix', reason.value.trim()) : toast('A decline needs a reason agents will read.', 'error')));
      }
    }
    bar.append(h('span', { class: 'spacer' }), button('Clear', 'ghost', () => { S.selected.clear(); S.panel = null; render(); }));
    return bar;
  }

  // --- the side peek ----------------------------------------------------------------
  function closePeek() { S.peek = null; S.detail = null; S.panel = null; }
  function peekView() {
    const i = S.detail?.id === S.peek && !S.detail.missing ? S.detail : null;
    const row = byID(S.peek), ids = walkable().map((x) => x.id), at = ids.indexOf(S.peek);
    // the identifier shrinks (ellipsis, the whole in its title) and, on a phone, takes a row of its own under
    // "← Issues", previous and next, so the header never makes the view wider than its frame
    const back = () => { closePeek(); render(); focusCursor(); };
    const head = h('div', { class: 'peek-head' },
      button('← Issues', 'ghost', back, { class: 'btn btn--ghost phone-only back', 'aria-label': 'Back to the issues' }),
      h('span', { class: 'peek-ident' },
        row || i ? icon((i ?? row).state) : null,
        h('code', { class: 'ident', title: S.peek }, S.peek),
        h('button', { type: 'button', class: 'btn btn--ghost btn--icon', 'aria-label': 'Copy the identifier', title: 'Copy', onclick: () => copy(S.peek) }, '⧉')),
      h('span', { class: 'spacer' }),
      button('↑', 'ghost', () => step(-1), { class: 'btn btn--ghost btn--icon', 'aria-label': 'Previous issue', disabled: at <= 0, title: 'Previous (k)' }),
      button('↓', 'ghost', () => step(1), { class: 'btn btn--ghost btn--icon', 'aria-label': 'Next issue', disabled: at < 0 || at >= ids.length - 1, title: 'Next (j)' }),
      button('×', 'ghost', back, { class: 'btn btn--ghost btn--icon wide-only', 'aria-label': 'Close the issue', title: 'Close (Esc)' }));
    if (S.detail?.missing) return h('div', { class: 'peek' }, head, h('p', { class: 'state-box' }, S.detail.missing));
    if (!i) return h('div', { class: 'peek' }, head, h('p', { class: 'state-box' }, 'Reading the issue…'));
    const mine = maintains(i.repo), can = allowed({ ...i, repo: i.repo });
    const editable = mine && i.state === 'new';
    const title = editable
      // a textarea that grows with its text, so a long title wraps instead of being cut (Enter still triages)
      ? h('textarea', { class: 'title-edit', id: 'title-edit', rows: 1, 'aria-label': 'Title', oninput: (e) => fitHeight(e.target),
          onkeydown: (e) => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); actions.triage(i, { title: e.target.value.trim().replace(/\s+/g, ' '), kind: kindValue() }); } } },
        i.title)
      : h('h2', { class: 'peek-title' }, i.title);
    const kindValue = () => app.querySelector('#kind-edit')?.value ?? i.kind;
    const props = h('dl', { class: 'props' },
      prop('State', stateLabel(i.state)),
      prop('Kind', editable ? h('select', { id: 'kind-edit', class: 'inline-select', 'aria-label': 'Kind' },
        KINDS.map((k) => h('option', { value: k, selected: k === i.kind }, k))) : h('span', { class: 'mono' }, i.kind)),
      prop('Component', h('span', { class: 'mono' }, i.component)),
      prop('Repository', h('span', { class: 'mono' }, ...breakAfterSlash(i.repo))),
      prop('Reports', h('span', { class: 'mono' }, String(i.count))),
      prop('Minutes', h('span', { class: 'mono' }, String(i.minutes))),
      i.filed ? prop('Filed', h('time', { datetime: i.filed, title: i.filed }, when(i.filed))) : null,
      i.updated ? prop('Updated', h('time', { datetime: i.updated, title: i.updated }, when(i.updated))) : null,
      i.fix ? prop('Fix', h('a', { href: i.fix.url, target: '_blank', rel: 'noopener' }, i.fix.sha.slice(0, 10))) : null,
      i.reason ? prop('Declined', h('span', {}, i.reason)) : null,
      row?.possibleDuplicateOf ? prop('May be', h('button', { type: 'button', class: 'linkish', onclick: () => openPeek(row.possibleDuplicateOf.id) },
        short(row.possibleDuplicateOf.id))) : null);
    const actionRow = h('div', { class: 'actions' });
    if (editable) actionRow.append(button('Triage', 'primary', () => actions.triage(i, { title: app.querySelector('#title-edit')?.value.trim(), kind: kindValue() })));
    if (can.close) actionRow.append(button('Close', 'primary', () => actions.close(i)));
    if (can.wontfix) actionRow.append(button('Decline…', 'secondary', () => { S.panel = 'decline'; render(); app.querySelector('#decline-reason')?.focus(); }));
    if (can.remove) actionRow.append(h('span', { class: 'spacer before-danger' }), button('Delete…', 'danger', () => { S.panel = 'delete'; render(); app.querySelector('#delete-reason')?.focus(); }));
    const sections = [head, title, props];
    if (actionRow.childNodes.length) sections.push(actionRow);
    if (S.panel === 'decline' && can.wontfix) sections.push(confirmBox('Decline', 'decline-reason', 'Why it will not be fixed: agents read this', 'secondary',
      (reason) => reason ? actions.wontfix(i, reason).then(() => { S.panel = null; render(); }) : toast('A decline needs a reason agents will read.', 'error')));
    if (S.panel === 'delete' && can.remove) sections.push(confirmBox('Delete for good', 'delete-reason', 'Reason, for the ledger (optional)', 'danger',
      (reason) => actions.remove(i, reason), `Delete ${i.id}? It leaves every list, its files are deleted, and its number is not reused.`));
    if (i.body) sections.push(h('div', { class: 'section' }, h('h3', {}, 'Report'), h('p', { class: 'prose' }, i.body)));
    if (i.workaround) sections.push(h('div', { class: 'section' }, h('h3', {}, 'Workaround'),
      h('p', { class: 'code' }, i.workaround.text), h('span', { class: 'by' }, `by ${i.workaround.by}`)));
    if (S.me) {
      const area = h('textarea', { class: 'area', name: 'text', required: true, 'aria-label': 'Workaround', placeholder: 'How you got past it' });
      sections.push(h('form', { class: 'section', onsubmit: (e) => { e.preventDefault();
        mutate(i.id, 'attach_workaround', { text: area.value }, { workaround: area.value }); } },
        h('h3', {}, i.workaround ? 'A better workaround' : 'Attach a workaround'), area,
        h('div', { class: 'actions' }, h('button', { class: 'btn btn--secondary', type: 'submit' }, 'Attach workaround'))));
    }
    if (Array.isArray(i.occurrences) && i.occurrences.length) sections.push(h('div', { class: 'section' }, h('h3', {}, `Reports (${i.occurrences.length})`),
      h('ul', { class: 'occurrences' }, i.occurrences.slice().reverse().map((o) => h('li', {},
        h('span', { class: 'mono' }, o.reporter ?? 'someone'), o.at ? h('time', { datetime: o.at, title: o.at }, when(o.at)) : null,
        o.commit ? h('span', { class: 'mono' }, o.commit.slice(0, 10)) : null,
        o.session ? h('a', { href: o.session, target: '_blank', rel: 'noopener' }, 'session') : null)))));
    if (Array.isArray(i.attachments) && i.attachments.length) sections.push(h('div', { class: 'section' }, h('h3', {}, 'Files'),
      h('ul', { class: 'filelist' }, i.attachments.map((a) => h('li', {}, a.name, h('span', { class: 'mono' }, `${a.kind} · ${a.size} B · ${a.scan.verdict}`))))));
    return h('div', { class: 'peek' }, sections);
  }
  const prop = (name, value) => [h('dt', {}, name), h('dd', {}, value)];
  // owner/name may break after its "/", not inside a word
  // (each part unbreakable, so "tinyactors-dev/tinyactors-napi" never breaks after a hyphen)
  const breakAfterSlash = (text) => String(text).split('/').flatMap((part, n, all) =>
    n < all.length - 1 ? [h('span', { class: 'nowrap' }, `${part}/`), h('wbr')] : [h('span', { class: 'nowrap' }, part)]);
  const fitHeight = (area) => { area.style.height = 'auto'; area.style.height = `${area.scrollHeight + area.offsetHeight - area.clientHeight}px`; };
  function confirmBox(label, id, placeholder, variant, go, warning) {
    const input = h('input', { id, 'aria-label': placeholder, placeholder });
    return h('div', { class: `confirm confirm--${variant}`, role: 'group', 'aria-label': label },
      warning ? h('p', {}, warning) : null, h('label', { class: 'ta-control' }, input),
      h('div', { class: 'actions' }, button(label, variant, () => go(input.value.trim())),
        button('Cancel', 'ghost', () => { S.panel = null; render(); })));
  }
  function when(iso) {
    const ms = Date.now() - Date.parse(iso);
    if (!Number.isFinite(ms)) return iso;
    const minutes = Math.round(ms / 60000);
    if (minutes < 1) return 'just now';
    if (minutes < 60) return `${minutes} min ago`;
    if (minutes < 1440) return `${Math.round(minutes / 60)} h ago`;
    if (minutes < 43200) return `${Math.round(minutes / 1440)} d ago`;
    return iso.slice(0, 10);
  }
  async function copy(text) {
    try { await navigator.clipboard.writeText(text); toast(`Copied ${text}`); }
    catch {
      const area = h('textarea', { class: 'offscreen' }, text);
      document.body.append(area); area.select();
      const ok = document.execCommand?.('copy');
      area.remove();
      toast(ok ? `Copied ${text}` : `The host does not allow copying: ${text}`, ok ? '' : 'error');
    }
  }

  // --- reporting --------------------------------------------------------------------
  function openReport() { if (!S.me) return; S.panel = 'report'; S.menu = null; render(); app.querySelector('#report-title')?.focus(); }
  function reportForm() {
    const chosen = project([...S.filters.repo][0]) ?? S.projects.find((p) => !p.archived);
    const field = (label, control) => h('label', { class: 'field' }, h('span', { class: 'label' }, label), control);
    const repo = h('select', { name: 'repo', 'aria-label': 'Repository', onchange: (e) => {
      const p = project(e.target.value); component.replaceChildren(...(p?.components ?? []).map((c) => h('option', { value: c }, c))); } },
      S.projects.filter((p) => !p.archived).map((p) => h('option', { value: p.repo, selected: p.repo === chosen?.repo }, p.repo)));
    const component = h('select', { name: 'component', 'aria-label': 'Component' },
      (chosen?.components ?? []).map((c) => h('option', { value: c, selected: S.filters.component.has(c) }, c)));
    const title = h('input', { id: 'report-title', name: 'title', required: true, 'aria-label': 'What went wrong, in a line', placeholder: 'What went wrong, in a line' });
    const body = h('textarea', { class: 'area', name: 'body', 'aria-label': 'What happened', placeholder: 'What you did, what happened, what you expected' });
    const kind = h('select', { name: 'kind', 'aria-label': 'Kind' }, h('option', { value: '' }, 'unsure'), KINDS.map((k) => h('option', { value: k }, k)));
    const minutes = h('input', { name: 'minutes', inputmode: 'numeric', 'aria-label': 'Minutes it cost', placeholder: 'Minutes' });
    const form = h('form', { class: 'peek', onsubmit: async (e) => {
      e.preventDefault();
      const args = { repo: repo.value, component: component.value, title: title.value.trim() };
      if (kind.value) args.kind = kind.value;
      if (body.value.trim()) args.body = body.value.trim();
      if (Number(minutes.value) > 0) args.minutes = Number(minutes.value);
      const { data, text, refused } = await call('report_issue', args);
      toast(text, refused ? 'error' : '');
      if (refused) return;
      S.panel = null;
      await load();
      if (data.id) openPeek(data.id);
    } },
      h('div', { class: 'peek-head' }, h('h2', { class: 'peek-title' }, 'Report an issue'), h('span', { class: 'spacer' }),
        button('×', 'ghost', () => { S.panel = null; render(); }, { 'aria-label': 'Cancel the report' })),
      h('div', { class: 'grid2' }, field('Repository', h('span', { class: 'ta-control' }, repo)), field('Component', h('span', { class: 'ta-control' }, component))),
      field('Title', h('span', { class: 'ta-control' }, title)),
      field('What happened', body),
      h('div', { class: 'grid2' }, field('Kind', h('span', { class: 'ta-control' }, kind)), field('Minutes it cost', h('span', { class: 'ta-control' }, minutes))),
      h('div', { class: 'actions' }, h('button', { class: 'btn btn--primary', type: 'submit' }, 'Report')));
    return form;
  }

  // --- toasts -----------------------------------------------------------------------
  // One message at a time, in the flow under the header: a toast fixed to the frame's bottom covered the
  // controls under it, and in a host that makes the frame as tall as its content it lands out of sight. It
  // stays until the next message or its ×: a timer taking it away would move the content under a finger.
  function toast(text, tone = '') {
    if (!text) return;
    regions.toasts ??= h('div', { class: 'toasts', role: 'status', 'aria-live': 'polite' });
    regions.toasts.replaceChildren(h('div', { class: 'toast', dataset: { tone } }, text),
      h('button', { type: 'button', class: 'btn btn--ghost btn--icon toast-x', 'aria-label': 'Dismiss the message',
        onclick: () => regions.toasts.replaceChildren() }, '×'));
  }

  // --- the keyboard -------------------------------------------------------------------
  function focusCursor(scroll = true) {
    const element = S.cursor ? app.querySelector(`.row[data-id="${CSS.escape(S.cursor)}"]`) : null;
    if (element) { element.focus({ preventScroll: !scroll }); if (scroll) element.scrollIntoView({ block: 'nearest' }); }
  }
  function moveCursor(delta) {
    const ids = walkable().map((i) => i.id);
    if (!ids.length) return;
    const at = ids.indexOf(S.cursor);
    S.cursor = at < 0 ? ids[0] : ids[Math.max(0, Math.min(ids.length - 1, at + delta))];
    if (S.peek && S.peek !== S.cursor) { openPeek(S.cursor); } else render();
    focusCursor();
  }
  function step(delta) { moveCursor(delta); }
  document.addEventListener('keydown', (e) => {
    const typing = e.target.closest?.('input, textarea, select') && e.target.type !== 'checkbox';
    if (e.key === 'Escape') {
      if (S.menu) { S.menu = null; render(); return; }
      if (typing) return;
      if (S.panel) { S.panel = null; render(); return; }
      if (S.peek) { closePeek(); render(); focusCursor(); return; }
      if (S.selected.size) { S.selected.clear(); render(); }
      return;
    }
    if (typing || e.metaKey || e.ctrlKey || e.altKey) return;
    const key = e.key;
    if (key === 'j' || key === 'ArrowDown') { e.preventDefault(); moveCursor(1); }
    else if (key === 'k' || key === 'ArrowUp') { e.preventDefault(); moveCursor(-1); }
    else if ((key === 'Enter' || key === 'o') && S.cursor && !e.target.closest?.('button:not(.row)')) { e.preventDefault(); openPeek(S.cursor); }
    else if (key === 'x' && S.cursor) { e.preventDefault(); select(S.cursor, 'toggle'); focusCursor(false); }
    else if (key === '/') { e.preventDefault(); toggleSearch(true); }
    else if (key === 'f') { e.preventDefault(); toggleMenu('filter'); }
    else if (key === 'c') { e.preventDefault(); openReport(); }
    else if (key === '?') { e.preventDefault(); toggleMenu('help'); }
  });
  document.addEventListener('click', (e) => {
    if (S.menu && !e.target.closest('.menu, #filter-button, #display-button, #more-button, [aria-label="Keyboard shortcuts"]')) { S.menu = null; render(); }
  });

  async function expand() {
    try { const result = await request('ui/request-display-mode', { mode: 'fullscreen' }); applyContext({ displayMode: result?.mode }); }
    catch (error) { toast(`The host kept the view as it is: ${error.message}`, 'error'); }
    render();
  }

  // --- start ------------------------------------------------------------------------
  let lastWidth = window.innerWidth;
  let lastHeight = 0;
  new ResizeObserver(() => {
    // the height the content needs (max-content, so content taller than the frame is not clamped to it); the
    // width stays the host's: reporting the frame's own width back could hold a frame wider than its card
    const html = document.documentElement, before = html.style.height;
    html.style.height = 'max-content';
    const height = Math.ceil(html.getBoundingClientRect().height);
    html.style.height = before;
    if (height !== lastHeight) { lastHeight = height; notify('ui/notifications/size-changed', { height }); }
    const crossed = (edge) => (window.innerWidth >= edge) !== (lastWidth >= edge);
    if (crossed(WIDE) || crossed(PHONE)) { lastWidth = window.innerWidth; render(); }
  }).observe(document.body);
  if (!document.documentElement.dataset.theme && window.matchMedia?.('(prefers-color-scheme: dark)').matches)
    document.documentElement.dataset.theme = 'dark';
  render();

  (async () => {
    try {
      const result = await request('ui/initialize', {
        appInfo: { name: 'tinyactors-support issues', version: '2' },
        appCapabilities: { availableDisplayModes: ['inline', 'fullscreen'] }, protocolVersion: PROTOCOL });
      applyContext(result?.hostContext);
      notify('ui/notifications/initialized', {});
    } catch (error) { S.loading = false; S.error = `The host did not start the app: ${error.message}`; render(); return; }
    const [projects, me] = await Promise.all([call('list_projects'), call('whoami')]);
    S.projects = projects.data.projects ?? [];
    S.me = me.refused || !me.data.login ? null : me.data;
    await load();
  })();
})();
