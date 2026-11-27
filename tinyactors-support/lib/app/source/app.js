// The issue list: an MCP App (the io.modelcontextprotocol/ui extension,
// protocol 2026-01-26). The host renders it for known_issues and get_issue;
// it talks to the host in JSON-RPC over postMessage, and every operation is
// a call of the server's own tools (tools/call through the host), as the
// signed-in user. What it may offer, it learns from whoami and
// list_projects; the server refuses whatever it should not do anyway.
(() => {
  'use strict';
  const PROTOCOL = '2026-01-26';
  const STATES = ['open', 'all', 'new', 'triaged', 'regressed', 'fixed', 'closed', 'wontfix'];
  const KINDS = ['docs', 'error', 'tooling', 'api', 'environment'];
  const OPEN = ['new', 'triaged', 'regressed'];

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
      return { data: result?.structuredContent ?? {}, text: result?.content?.find((c) => c.type === 'text')?.text ?? '',
               failed: !!result?.isError };
    } catch (error) {
      return { data: { error: error.message }, text: error.message, failed: true };
    }
  }

  // --- what the view knows ----------------------------------------------------
  const view = {
    projects: [], me: null, issues: [], issue: null, page: 'list',
    filter: { repo: '', component: '', state: 'open' }, context: {}, confirming: false,
  };
  const project = (repo) => view.projects.find((p) => p.repo === repo || p.slug === repo);
  const maintains = (repo) => !!view.me && !!project(repo) && view.me.maintainerOf.includes(project(repo).repoID);

  // --- the host's context: theme, size, display mode -----------------------
  function applyContext(context = {}) {
    view.context = { ...view.context, ...context };
    const theme = view.context.theme;
    if (theme === 'dark' || theme === 'light') document.documentElement.dataset.theme = theme;
    const dimensions = view.context.containerDimensions ?? {};
    document.documentElement.style.maxHeight = dimensions.maxHeight ? `${dimensions.maxHeight}px` : '';
  }
  handlers['ui/notifications/host-context-changed'] = (params) => applyContext(params);
  handlers['ui/notifications/tool-input'] = ({ arguments: args = {} }) => {
    if (args.repo) view.filter.repo = project(args.repo)?.repo ?? args.repo;
    if (args.component) view.filter.component = args.component;
    if (args.state) view.filter.state = args.state;
    if (args.id) view.page = 'issue';
  };
  handlers['ui/notifications/tool-result'] = (result) => {
    const data = result?.structuredContent ?? {};
    if (Array.isArray(data.issues)) { view.issues = data.issues; view.page = 'list'; }
    else if (data.answer === 'issue') { view.issue = data; view.page = 'issue'; }
    render();
  };

  // --- reading -------------------------------------------------------------------
  async function loadList() {
    const args = { state: view.filter.state };
    if (view.filter.repo) args.repo = view.filter.repo;
    if (view.filter.component) args.component = view.filter.component;
    const { data, text, failed } = await call('known_issues', args);
    if (failed) return say(text, true);
    view.issues = data.issues ?? [];
    if (data.sinceYourLastVisit?.length) say(text);
    render();
  }
  async function open(id) {
    const { data, text, failed } = await call('get_issue', { id });
    if (failed || data.answer !== 'issue') return say(text, true);
    view.issue = data; view.page = 'issue'; view.confirming = false;
    render();
  }
  // A mutation: the tool's own line goes to the status, then the view reloads.
  async function act(name, args, then) {
    const { data, text, failed } = await call(name, args);
    const refused = failed || ['refused', 'failed'].includes(data.answer);
    say(text, refused);
    if (!refused) await then(data);
    return !refused;
  }

  // --- drawing ------------------------------------------------------------------
  const root = document.getElementById('view');
  const status = document.getElementById('status');
  function say(text, error = false) { status.textContent = text ?? ''; status.dataset.tone = error ? 'error' : ''; }
  function h(tag, attributes = {}, ...children) {
    const element = document.createElement(tag);
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
  const button = (label, variant, onclick, extra = {}) => h('button', { class: `btn btn--${variant}`, type: 'button', onclick, ...extra }, label);
  const select = (name, options, value, onchange, label) => h('label', { class: 'ta-control', title: label },
    h('select', { name, 'aria-label': label, onchange: (e) => onchange(e.target.value) },
      options.map(([v, text]) => h('option', { value: v, selected: v === value }, text))));
  const input = (name, placeholder, value = '') => h('label', { class: 'ta-control' }, h('input', { name, placeholder, value, 'aria-label': placeholder }));
  const field = (label, control) => h('div', { class: 'field' }, h('span', { class: 'label' }, label), control);
  const valueOf = (form, name) => form.querySelector(`[name="${name}"]`)?.value ?? '';

  function render() {
    document.getElementById('count').textContent = view.page === 'list' ? `${view.issues.length} ${view.filter.state}` : '';
    document.getElementById('report').hidden = !view.me;
    renderFilters();
    root.replaceChildren(view.page === 'issue' && view.issue ? detail(view.issue) : view.page === 'new' ? reportForm() : list());
  }

  function renderFilters() {
    const filters = document.getElementById('filters');
    const chosen = project(view.filter.repo);
    filters.hidden = view.page !== 'list';
    if (filters.hidden) return filters.replaceChildren();
    filters.replaceChildren(
      select('repo', [['', 'every repository'], ...view.projects.map((p) => [p.repo, p.repo])], view.filter.repo,
        (v) => { view.filter.repo = v; view.filter.component = ''; loadList(); }, 'Repository'),
      select('component', [['', 'every component'], ...(chosen?.components ?? []).map((c) => [c, c])], view.filter.component,
        (v) => { view.filter.component = v; loadList(); }, 'Component'),
      select('state', STATES.map((s) => [s, s]), view.filter.state, (v) => { view.filter.state = v; loadList(); }, 'State'),
      button('Refresh', 'ghost', () => loadList(), { 'aria-label': 'Refresh' }));
  }

  function list() {
    if (view.issues.length === 0) return h('div', { class: 'list' }, h('p', { class: 'empty' }, 'No issues match.'));
    return h('div', { class: 'list', role: 'list' }, view.issues.map((i) => h('button', {
      class: 'row', type: 'button', role: 'listitem', dataset: { id: i.id }, onclick: () => open(i.id),
      title: i.workaround ? `Workaround: ${i.workaround}` : undefined },
      h('span', { class: 'dot', dataset: { state: i.state }, title: i.state }),
      h('span', { class: 'id' }, i.id),
      h('span', { class: 'title' }, i.title, h('span', { class: 'tag' }, i.component)),
      h('span', { class: `cost${i.workaround ? ' has-workaround' : ''}`, title: `${i.count} ${i.count === 1 ? 'report' : 'reports'} · ${i.minutes} min` },
        `${i.count}× · ${i.minutes}m`))));
  }

  function detail(i) {
    const mine = maintains(i.repo);
    const isOpen = OPEN.includes(i.state);
    const cards = [h('div', { class: 'card' },
      h('div', { class: 'actions' }, button('← Issues', 'ghost', () => { view.page = 'list'; loadList(); }),
        h('span', { class: 'spacer' }), h('span', { class: 'badge', dataset: { state: i.state } }, h('span', { class: 'dot' }), i.state)),
      h('span', { class: 'ident' }, i.id),
      h('h2', {}, i.title),
      h('div', { class: 'meta' },
        h('span', {}, 'repository ', h('b', {}, i.repo)), h('span', {}, 'component ', h('b', {}, i.component)),
        h('span', {}, 'kind ', h('b', {}, i.kind)), h('span', {}, h('b', {}, String(i.count)), i.count === 1 ? ' report' : ' reports'),
        h('span', {}, h('b', {}, String(i.minutes)), ' min')),
      i.body ? h('p', { class: 'prose' }, i.body) : null,
      i.fix ? h('p', {}, 'Fixed in ', h('a', { href: i.fix.url, target: '_blank', rel: 'noopener' }, i.fix.sha.slice(0, 10))) : null,
      i.reason ? h('p', {}, 'Declined: ', i.reason) : null)];
    if (i.workaround) cards.push(h('div', { class: 'card' }, h('h3', {}, 'Workaround'),
      h('p', { class: 'code' }, i.workaround.text), h('span', { class: 'label' }, `by ${i.workaround.by}`)));
    if (Array.isArray(i.attachments) && i.attachments.length) cards.push(h('div', { class: 'card' }, h('h3', {}, 'Files'),
      h('ul', { class: 'files' }, i.attachments.map((a) => h('li', {}, a.name,
        h('span', { class: 'mono' }, `${a.kind} · ${a.size} B · ${a.scan.verdict}`))))));
    if (view.me) {
      const form = h('form', { class: 'card', onsubmit: (e) => { e.preventDefault();
        act('attach_workaround', { id: i.id, text: valueOf(form, 'text') }, () => open(i.id)); } },
        h('h3', {}, 'Attach a workaround'),
        h('textarea', { class: 'area', name: 'text', required: true, 'aria-label': 'Workaround', placeholder: 'How you got past it' }),
        h('div', { class: 'actions' }, h('button', { class: 'btn btn--secondary', type: 'submit' }, 'Attach workaround')));
      cards.push(form);
    }
    if (mine) cards.push(maintainerCard(i, isOpen));
    return h('div', { class: 'app-body', style: 'display:flex;flex-direction:column;gap:var(--space-3)' }, cards);
  }

  function maintainerCard(i, isOpen) {
    const card = h('div', { class: `card${view.confirming ? ' confirm' : ''}` }, h('h3', {}, 'Maintainer'));
    if (view.confirming) {
      card.append(h('p', {}, `Delete ${i.id} for good? It leaves every list, its files are deleted, and its number is not reused.`),
        input('reason', 'Reason, for the ledger'),
        h('div', { class: 'actions' },
          button('Delete for good', 'danger', () => act('delete_issue', { id: i.id, reason: valueOf(card, 'reason') || undefined },
            () => { view.confirming = false; view.page = 'list'; return loadList(); })),
          button('Cancel', 'ghost', () => { view.confirming = false; render(); })));
      return card;
    }
    if (i.state === 'new') card.append(h('div', { class: 'grid2' },
      field('Kind', select('kind', KINDS.map((k) => [k, k]), i.kind, () => {}, 'Kind')),
      field('Title', input('title', 'A better title', i.title))),
      h('div', { class: 'actions' }, button('Triage', 'primary', () => act('triage',
        { id: i.id, kind: valueOf(card, 'kind'), title: valueOf(card, 'title') || undefined }, () => open(i.id)))));
    if (isOpen) card.append(input('reason', 'Why it will not be fixed'), h('div', { class: 'actions' },
      button('Decline', 'secondary', () => {
        const reason = valueOf(card, 'reason');
        if (!reason) return say('A decline needs a reason agents will read.', true);
        return act('wontfix', { id: i.id, reason }, () => open(i.id));
      })));
    const row = h('div', { class: 'actions' });
    if (i.state === 'fixed') row.append(button('Close', 'primary', () => act('close', { id: i.id }, () => open(i.id))));
    row.append(h('span', { class: 'spacer' }), button('Delete…', 'danger', () => { view.confirming = true; render(); }));
    card.append(row);
    return card;
  }

  function reportForm() {
    const repo = project(view.filter.repo) ?? view.projects.find((p) => !p.archived);
    const form = h('form', { class: 'card', onsubmit: async (e) => {
      e.preventDefault();
      const args = { repo: valueOf(form, 'repo'), component: valueOf(form, 'component'), title: valueOf(form, 'title'),
                     kind: valueOf(form, 'kind') || undefined };
      const body = valueOf(form, 'body'), minutes = Number(valueOf(form, 'minutes'));
      if (body) args.body = body;
      if (minutes > 0) args.minutes = minutes;
      await act('report_issue', args, (data) => data.id ? open(data.id) : loadList());
    } },
      h('div', { class: 'actions' }, button('← Issues', 'ghost', () => { view.page = 'list'; render(); })),
      h('h2', {}, 'Report an issue'),
      h('div', { class: 'grid2' },
        field('Repository', select('repo', view.projects.filter((p) => !p.archived).map((p) => [p.repo, p.repo]), repo?.repo,
          (v) => { view.filter.repo = v; render(); }, 'Repository')),
        field('Component', select('component', (repo?.components ?? []).map((c) => [c, c]), view.filter.component, () => {}, 'Component'))),
      field('Title', input('title', 'What went wrong, in a line')),
      field('What happened', h('textarea', { class: 'area', name: 'body', 'aria-label': 'What happened',
        placeholder: 'What you did, what happened, what you expected' })),
      h('div', { class: 'grid2' },
        field('Kind', select('kind', [['', 'unsure'], ...KINDS.map((k) => [k, k])], '', () => {}, 'Kind')),
        field('Minutes it cost', input('minutes', 'Minutes'))),
      h('div', { class: 'actions' }, h('button', { class: 'btn btn--primary', type: 'submit' }, 'Report')));
    return form;
  }

  // --- start ------------------------------------------------------------------------
  document.getElementById('report').addEventListener('click', () => { view.page = 'new'; render(); });
  new ResizeObserver(() => {
    const height = Math.ceil(document.documentElement.getBoundingClientRect().height);
    notify('ui/notifications/size-changed', { width: Math.ceil(window.innerWidth), height });
  }).observe(document.body);
  if (!document.documentElement.dataset.theme && window.matchMedia?.('(prefers-color-scheme: dark)').matches)
    document.documentElement.dataset.theme = 'dark';

  (async () => {
    try {
      const result = await request('ui/initialize', {
        appInfo: { name: 'tinyactors-support issues', version: '1' },
        appCapabilities: { availableDisplayModes: ['inline', 'fullscreen'] }, protocolVersion: PROTOCOL });
      applyContext(result?.hostContext);
      notify('ui/notifications/initialized', {});
    } catch (error) { say(`The host did not start the app: ${error.message}`, true); return; }
    const [projects, me] = await Promise.all([call('list_projects'), call('whoami')]);
    view.projects = projects.data.projects ?? [];
    view.me = me.failed || !me.data.login ? null : me.data;
    if (view.page === 'issue' && view.issue) render();
    else if (view.issues.length) render();
    else await loadList();
  })();
})();
