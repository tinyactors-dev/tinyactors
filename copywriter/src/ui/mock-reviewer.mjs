// A fake Reviewer for developing and testing the panel without the actor
// host or Gemini: the facade of AGENTS.md ("The facade: Reviewer") over a
// plain object, with sample state for www.tinyactors.dev and simulated
// delays for the model.
//
//   const reviewer = createMockReviewer({ hasKey: true, delay: 1 });
//   reviewer.setSelection('Actors as a library');   // what the page would report
//
// `delay` scales every simulated wait (0 makes them immediate macrotasks).

import { buildPacket } from '../core/packet.mjs';
import { siteGrid } from '../core/grid.mjs';

const MINUTE = 60_000;
const SITE = 'https://www.tinyactors.dev';

// The plan of the page ("Revise page"): a rewrite that fits the pitch, a
// need of an explanation linked to its page (routed by the core), an add, a
// move to a how-to, and a new page.
function samplePlan() {
  const crit = (needID, page, text) => ({ needID, page, text });
  return {
    items: [
      { id: 'p1', action: 'rewrite', needs: ['tech-lead.decide'], target: 'here', original: 'An actor takes about 530 bytes in Node.js.', anchored: true,
        text: 'An actor takes about 530 bytes in Node.js, measured on the memory page.', reason: 'The lead decides on measured numbers.',
        criteria: [crit('tech-lead.decide', `${SITE}/`, 'The home page says where the 530 bytes were measured')], audiences: ['Tech lead who approves dependencies'], decision: null },
      { id: 'p2', action: 'link', needs: ['tech-lead.host-dies'], target: `${SITE}/guides/snapshots/`, targetTitle: 'Snapshots', anchored: false,
        text: 'What happens to state when the host dies? See Snapshots.', targetText: 'Say what a snapshot keeps when the host dies.',
        reason: 'tech-lead.host-dies belongs on a how-to page: linked to /guides/snapshots/.', conflict: 'tech-lead.host-dies belongs on a how-to page: linked to /guides/snapshots/.', routed: true,
        criteria: [crit('tech-lead.host-dies', `${SITE}/`, 'The home page links to /guides/snapshots/ for tech-lead.host-dies'), crit('tech-lead.host-dies', `${SITE}/guides/snapshots/`, '/guides/snapshots/ answers tech-lead.host-dies')],
        audiences: ['Tech lead who approves dependencies'], decision: null },
      { id: 'p3', action: 'add', needs: ['tech-lead.maintainer', 'tech-lead.decide'], target: 'here', anchored: false, where: 'Like SQLite, but for actors',
        text: 'tinyactors is maintained by its author, in the open on GitHub.', reason: 'The lead asks who maintains it.',
        criteria: [crit('tech-lead.maintainer', `${SITE}/`, 'The home page says who maintains tinyactors')], audiences: ['Tech lead who approves dependencies'], decision: null },
      { id: 'p4', action: 'move', needs: ['tech-lead.first-chart'], target: `${SITE}/guides/charts/`, targetTitle: 'Writing charts', original: 'Get started in minutes', anchored: true,
        text: 'Write your first chart.', reason: 'A first chart is a how-to.', conflict: 'A first chart is a how-to.',
        criteria: [crit('tech-lead.first-chart', `${SITE}/guides/charts/`, '/guides/charts/ answers tech-lead.first-chart')], audiences: ['Tech lead who approves dependencies'], decision: null },
    ],
    newPages: [
      { id: 'n1', title: 'When to use tinyactors', kind: 'explanation', audiences: ['tech-lead'], outline: ['Use it when', 'Do not use it when'], needs: ['tech-lead.when-to-use'],
        criteria: [crit('tech-lead.when-to-use', 'new', 'A new explanation page "When to use tinyactors" answers tech-lead.when-to-use')], reason: 'No page says when not to.', decision: null },
    ],
  };
}

const VOICE = `You write copy for www.tinyactors.dev.

- Headlines and headings: 1 to 8 words, sentence case, no period.
- A paragraph is one or two sentences of at most 21 words.
- The product or the reader's code is the subject.
- No metaphors, no hype words, no adjectives that do not measure something.`;

const REWRITES = {
  default: [
    ['Run millions of actors in one process', 'States the measured claim instead of a category.'],
    ['Actors in C and JavaScript, one semantics', 'Names both languages; the reader sees the scope at once.'],
    ['Spawn, send, assert the state', 'Three verbs in the order a developer uses them.'],
  ],
};

// a site audience: from the file the page links (AGENTS.md, "Pages say what they are for")
const LEAD = {
  id: 'lead', name: 'Tech lead who approves dependencies', description: 'Decides whether the team may depend on tinyactors.',
  source: 'site', url: 'https://www.tinyactors.dev/audiences/tech-lead.xml', siteID: 'tech-lead', diverged: false,
  wants: 'An honest "use it when"; the license', tasks: 'Decide yes or no, check the license',
  objections: '"Who maintains it?" "What happens when the host dies?"',
};

// The tech lead's reading of the page: its checklist and two findings.
function sampleReading(now) {
  return {
    audienceID: 'lead', state: 'done', at: now - 9 * MINUTE, seconds: 41,
    items: [
      { field: 'wants', text: 'An honest "use it when"', status: 'missing', explanation: 'The page never says when not to use it.', needID: 'tech-lead.when-to-use', kind: 'explanation' },
      { field: 'wants', text: 'The license', status: 'answered', explanation: 'MIT, in the footer.', anchor: { quote: { exact: 'MIT licensed.', prefix: '', suffix: '' }, selector: 'footer p' }, needID: 'tech-lead.license', kind: 'reference' },
      { field: 'tasks', text: 'Decide yes or no', status: 'partial', explanation: 'Facts, but no limits.', needID: 'tech-lead.decide', kind: 'pitch' },
      { field: 'tasks', text: 'check the license', status: 'answered', explanation: 'MIT.', anchor: { quote: { exact: 'MIT licensed.', prefix: '', suffix: '' }, selector: 'footer p' }, needID: 'tech-lead.check-portability', kind: 'reference' },
      { field: 'tasks', text: 'npm install and run one chart', status: 'partial', explanation: 'The command is there, the chart is not.', needID: 'tech-lead.first-chart', kind: 'tutorial' },
      { field: 'objections', text: '"Who maintains it?"', status: 'missing', explanation: 'No maintainer is named.', needID: 'tech-lead.maintainer', kind: 'pitch' },
      { field: 'objections', text: '"What happens when the host dies?"', status: 'partial', explanation: 'Snapshots, but nothing on a crash.', needID: 'tech-lead.host-dies', kind: 'explanation' },
    ],
    findingIDs: ['f1', 'f2'],
  };
}

function sampleFindings(now) {
  const at = (minutes) => now - minutes * MINUTE;
  return [
    {
      id: 'f1', state: 'suggested', anchorFound: true, source: 'audience', original: 'An actor takes about 530 bytes in Node.js.',
      note: '', chosenID: null, messages: [], audiences: [LEAD.name],
      suggestions: [{ id: 's1', text: 'An actor takes about 530 bytes in Node.js, measured with the benchmark on the memory page.', rationale: 'Measured how, and on what?', after: 0 }],
      finding: { audienceID: 'lead', kind: 'doubt', explanation: 'I cannot size servers from 530 bytes without knowing how it was measured.' },
      createdAt: at(9), updatedAt: at(9),
    },
    {
      id: 'f2', state: 'draft', anchorFound: true, pageLevel: true, source: 'audience', original: 'Add a "when not to use it" section',
      note: '', chosenID: null, messages: [], audiences: [LEAD.name], suggestions: [],
      finding: { audienceID: 'lead', kind: 'missing', explanation: 'I cannot tell when to say no.', section: 'Like SQLite, but for actors' },
      createdAt: at(9), updatedAt: at(9),
    },
  ];
}

function sample(now) {
  const at = (minutes) => now - minutes * MINUTE;
  return [
    {
      id: 'a4', state: 'suggested', anchorFound: true,
      original: 'A tiny, embeddable actor runtime for building reliable systems',
      note: 'Too generic. Say what a developer gets.',
      suggestions: [
        { id: 's41', text: 'Run millions of actors in one process', rationale: 'States the measured claim instead of a category.', after: 2 },
        { id: 's42', text: 'Statecharts that run anywhere C runs', rationale: 'Names the model and the reach, 7 words.', after: 2 },
      ],
      chosenID: null,
      messages: [
        { role: 'user', text: 'What would a developer care about here?', at: at(6) },
        { role: 'model', text: 'The headline names a category. The page\'s strongest fact is **4 million actors in 1.5 GB**, so lead with the capacity:\n\n- one rewrite states the number\n- one names `SCXML` statecharts', at: at(6),
          thoughts: '**Finding the strongest fact**\nThe page measures 4 million actors in 1.5 GB; the headline says none of it.', seconds: 9 },
      ],
      createdAt: at(7), updatedAt: at(6),
    },
    {
      id: 'a3', state: 'previewing', anchorFound: true,
      original: 'Get started in minutes',
      note: '',
      suggestions: [{ id: 's31', text: 'Install the package', rationale: 'A verb and an object, as the voice asks.' }],
      chosenID: 's31',
      messages: [],
      createdAt: at(30), updatedAt: at(12),
    },
    {
      id: 'a2', state: 'kept', anchorFound: true,
      original: 'Blazing-fast message passing between your actors, powered by a revolutionary new design.',
      note: 'Hype words.',
      suggestions: [{ id: 's21', text: 'Hundreds of thousands of events per second, in one thread.', rationale: 'A number replaces two adjectives.' }],
      chosenID: 's21',
      messages: [],
      createdAt: at(80), updatedAt: at(65),
    },
    {
      id: 'a1', state: 'draft', anchorFound: false,
      original: 'Supervision trees keep your system alive when things go wrong',
      note: 'Check this against the docs: is restart in core?',
      suggestions: [], chosenID: null, messages: [],
      createdAt: at(200), updatedAt: at(190),
    },
  ];
}

// The site map of the sample site: six pages, their kinds and audiences.
const SITE_MAP = [
  ['/', 'tinyactors: like SQLite, but for actors', 'pitch', ['agent-platform', 'tech-lead', 'workflow-tester']],
  ['/chat/', 'Build a team chat with tinyactors', 'tutorial', ['agent-platform', 'workflow-tester']],
  ['/guides/testing/', 'Testing charts', 'how-to', ['workflow-tester', 'tech-lead']],
  ['/performance/', 'Performance', 'reference', ['tech-lead', 'agent-platform']],
  ['/about/', 'Why tinyactors exists', 'explanation', ['tech-lead']],
  ['/guides/snapshots/', 'Snapshots', 'how-to', ['tech-lead', 'agent-platform']],
].map(([path, title, kind, audiences]) => ({ url: `${SITE}${path}`, path, title, kind, audiences, description: '' }));

// The tech lead's readings of the other pages, for the site view.
function siteReadings(now) {
  const item = (needID, status, explanation) => ({ field: 'wants', text: needID, needID, status, explanation });
  return [
    { page: `${SITE}/about/`, audienceID: 'lead', state: 'done', at: now - 30 * MINUTE, items: [
      item('tech-lead.when-to-use', 'partial', 'Why it exists, not when not to use it.'), item('tech-lead.maintainer', 'answered', 'Names the maintainer.'),
      item('tech-lead.host-dies', 'missing', '')] },
    { page: `${SITE}/guides/snapshots/`, audienceID: 'lead', state: 'done', at: now - 25 * MINUTE, items: [
      item('tech-lead.host-dies', 'answered', 'A snapshot restores every actor.'), item('tech-lead.decide', 'missing', '')] },
  ];
}

// A rewrite per audience, when several are active: by the audience's id, and one for everyone.
const PER_AUDIENCE = {
  lead: ['A runtime you embed: one C core, MIT licensed', 'Names what the lead checks first: the core and the license.'],
  student: ['Statecharts you can run in a page, in one example', 'One small example is what the student wants.'],
  dev: ['Test a week of timeouts in milliseconds', 'The developer wants the simulated clock.'],
};

export function createMockReviewer({ hasKey = true, delay = 1, empty = false, status = 'ready', read = true, suggest = false, diverged = false, site = true, changed = false } = {}) {
  const now = Date.now();
  let seq = 100;
  const listeners = new Set();
  const log = [];
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms * delay));
  const id = (prefix) => `${prefix}${++seq}`;
  let readingMS = 1200;  // how long a reading takes, before the delay scales it (setReadingTime)
  let exported = null;   // the last export's CHANGES.md, built by the real packet code

  let state = {
    status,
    page: {
      url: 'https://www.tinyactors.dev/', title: 'Actors as a library', kind: 'pitch', mainAudienceID: read ? 'lead' : null,
      audienceSuggestion: suggest ? { state: 'ready', audienceIDs: read ? ['lead', 'student'] : ['student'] } : null,
      changed: changed && read,
    },
    selection: null,
    picking: false,
    settings: {
      hasKey, rememberKey: false, model: 'gemini-4-hydrogen', thinking: 'high',
      length: 'short', tools: { search: false, url: true, code: false },
      models: [
        { id: 'gemini-4-hydrogen', label: 'Gemini 4 Hydrogen' },
        { id: 'gemini-3.8-flash', label: 'Gemini 3.8 Flash' },
        { id: 'gemini-3.5-pro', label: 'Gemini 3.5 Pro' },
      ],
      voice: { instructions: VOICE, custom: false },
      audiences: [
        { id: 'student', name: 'Student', description: 'Learns statecharts for a course.', wants: 'One small example.' },
        ...(read ? [{ ...LEAD, diverged }] : []),
      ],
      audienceIDs: empty || !read ? [] : ['lead'],
    },
    annotations: empty ? [] : [...(read ? sampleFindings(now) : []), ...sample(now)],
    plan: null,
    criteria: changed && read ? { at: null, items: [
      { id: 'c1', text: 'The home page links to /guides/snapshots/ for tech-lead.host-dies', needID: 'tech-lead.host-dies', page: `${SITE}/`, from: { page: `${SITE}/`, itemID: 'p2' }, met: null },
      { id: 'c2', text: 'The home page says who maintains tinyactors', needID: 'tech-lead.maintainer', page: `${SITE}/`, from: { page: `${SITE}/`, itemID: 'p3' }, met: null },
    ] } : null,
    site: { state: 'none', pages: [], audiences: [], progress: null, running: [], failures: [], started: null, at: null },
    readings: empty || !read ? [] : [sampleReading(now)],
    activeID: null,
    chat: {
      id: 'c1', audienceID: null, messages: [], busy: false,
      conversations: [{ id: 'c0', title: 'Which claims need a number?', at: now - 3 * 60 * MINUTE }],
    },
    recording: { state: 'idle', target: null },
    totals: { kept: 0, open: 0, pages: 3 },
    exporting: 'idle',
  };

  function totals(annotations) {
    const kept = annotations.filter((a) => a.state === 'kept').length;
    const open = annotations.filter((a) => !['kept', 'dismissed'].includes(a.state)).length;
    return { kept: kept + 2, open, pages: state.totals.pages };   // two kept on other pages
  }

  // the page chat's conversations: id → messages (the open one is state.chat)
  const conversations = new Map([['c0', [
    { role: 'user', text: 'Which claims need a number?', at: now - 3 * 60 * MINUTE },
    { role: 'model', text: '“Blazing-fast” needs one: the page measures events per second.', at: now - 3 * 60 * MINUTE },
  ]]]);
  const whose = new Map(); // conversation id → the audience it is held with
  function chatSet(patch) {
    const chat = { ...state.chat, ...patch };
    if (chat.audienceID) whose.set(chat.id, chat.audienceID);
    if (chat.messages.length) conversations.set(chat.id, chat.messages);
    chat.conversations = [...conversations.entries()].filter(([, m]) => m.length)
      .map(([cid, m]) => ({ id: cid, title: m[0].text.slice(0, 60), at: m.at(-1).at, audienceID: whose.get(cid) ?? null }))
      .sort((a, b) => b.at - a.at);
    return chat;
  }

  function set(patch) {
    const next = { ...state, ...patch };
    if (patch.annotations) next.totals = totals(next.annotations);
    state = Object.freeze(next);
    for (const fn of listeners) fn(state);
  }

  function update(annotationID, fn) {
    const t = Date.now();
    set({ annotations: state.annotations.map((a) => (a.id === annotationID ? { ...fn(a), updatedAt: t } : a)) });
  }

  const get = (annotationID) => state.annotations.find((a) => a.id === annotationID);

  // Plan items decided: an accepted rewrite, cut, add or link is an annotation here.
  function decide(ids, decision) {
    const t = Date.now();
    const made = [];
    const mark = (x) => {
      if (x.decision || !ids.includes(x.id)) return x;
      const y = { ...x, decision };
      if (decision === 'accepted' && x.action !== 'new' && (x.target === 'here' || x.action === 'link') && x.action !== 'move') {
        y.annotationID = `${x.id}-a`;
        made.push({
          id: y.annotationID, state: 'suggested', anchorFound: true, source: 'plan', original: x.anchored ? x.original : x.reason,
          ...(x.anchored ? {} : { pageLevel: true }), note: '', chosenID: null, messages: [], audiences: x.audiences,
          suggestions: [{ id: 's1', text: x.action === 'cut' ? '' : x.text, rationale: x.reason, after: 0 }],
          plan: { itemID: x.id, action: x.action, needs: x.needs, criteria: x.criteria, target: x.target, where: x.where ?? null },
          createdAt: t, updatedAt: t,
        });
      }
      return y;
    };
    const plan = { ...state.plan, items: state.plan.items.map(mark), newPages: state.plan.newPages.map(mark) };
    set({ plan, annotations: [...made, ...state.annotations] });
  }

  async function ask(annotationID, text) {
    if (!state.settings.hasKey) {
      update(annotationID, (a) => ({ ...a, state: 'failed', error: 'No API key. Add one in Settings.' }));
      return;
    }
    const before = get(annotationID).state;
    update(annotationID, (a) => ({
      ...a, state: 'asking', error: undefined,
      messages: text ? [...a.messages, { role: 'user', text, at: Date.now() }] : a.messages,
    }));
    await wait(900);
    if (!get(annotationID)) return;
    if (/fail/i.test(text ?? '')) {
      update(annotationID, (a) => ({ ...a, state: 'failed', error: 'Gemini answered 503: the model is overloaded. Try again.' }));
      return;
    }
    // several audiences active, and not a finding: one rewrite each, and one for everyone
    const active = state.settings.audienceIDs.filter((x) => state.settings.audiences.some((a) => a.id === x));
    if (active.length > 1 && !get(annotationID).finding) {
      update(annotationID, (a) => {
        const messages = [...a.messages, { role: 'model', text: 'One for each reader, and one for all of them.', at: Date.now(), seconds: 1 }];
        const n = a.suggestions.length;
        const brought = [
          ...active.map((x, i) => ({ id: `s${n + i + 1}`, text: (PER_AUDIENCE[x] ?? [`For ${x}: ${a.original}`, ''])[0], rationale: (PER_AUDIENCE[x] ?? ['', 'Their need first.'])[1], audienceID: x })),
          { id: `s${n + active.length + 1}`, text: 'Run millions of actors in one process', rationale: 'The measured claim serves every reader.', audienceID: null },
        ];
        return { ...a, state: before === 'previewing' ? 'previewing' : 'suggested', messages, suggestions: [...a.suggestions, ...brought.map((x) => ({ ...x, after: messages.length }))] };
      });
      return;
    }
    const pool = REWRITES.default;
    const fresh = pool.map(([t, r]) => ({ id: id('s'), text: t, rationale: r }))
      .filter((s) => !get(annotationID).suggestions.some((o) => o.text === s.text)).slice(0, 2);
    const reply0 = fresh.length
      ? `Two options that follow the voice. The first leads with **the measured claim**; the second keeps the verb first.`
      : 'These already follow the voice. Tell me what to emphasise and I will try again.';
    update(annotationID, (a) => {
      const messages = [...a.messages, { role: 'model', text: reply0, at: Date.now(), thoughts: '**Weighing the options**\nThe voice wants the measured claim first.', seconds: 1 }];
      const brought = fresh.length ? fresh : [{ id: id('s'), text: `${a.original.split(' ').slice(0, 5).join(' ')}`, rationale: 'Cut to the first five words.' }];
      return {
        ...a,
        state: before === 'previewing' ? 'previewing' : (before === 'kept' ? 'kept' : 'suggested'),
        suggestions: [...a.suggestions, ...brought.map((s) => ({ ...s, after: messages.length }))],
        messages,
      };
    });
  }

  // A reply of the reader the open conversation is held with: a
  // correction updates the audience, "missing" notes a gap, anything else
  // answers from the page.
  async function readerReply(text) {
    const audience = state.settings.audiences.find((a) => a.id === state.chat.audienceID);
    const say = (message) => set({ chat: chatSet({ busy: false, messages: [...state.chat.messages, { role: 'model', at: Date.now(), ...message }] }) });
    if (/actually|you'd|you would|erlang/i.test(text)) {
      const after = `${audience.knows ? `${audience.knows.replace(/\.$/, '')}; ` : ''}Erlang and OTP, well.`;
      const changes = [{ field: 'knows', before: audience.knows ?? '', after }];
      set({ settings: { ...state.settings, audiences: state.settings.audiences.map((a) => (a.id === audience.id ? { ...a, knows: after } : a)) } });
      say({ text: 'Then "actors as a library" is a claim I can check against OTP, and the page gives me no comparison.', audiences: [{ name: audience.name, action: 'update', id: audience.id, changes }] });
      return;
    }
    if (/missing/i.test(text)) {
      const t = Date.now();
      const gap = {
        id: id('g'), state: 'draft', anchorFound: true, pageLevel: true, source: 'audience', original: 'Name who maintains it',
        note: '', chosenID: null, messages: [], audiences: [audience.name], suggestions: [],
        finding: { audienceID: audience.id, kind: 'missing', explanation: 'I will not approve a dependency with no named owner.' },
        createdAt: t, updatedAt: t,
      };
      set({ annotations: [gap, ...state.annotations] });
      say({ text: 'Who maintains it. The page doesn\'t tell me whether anyone is paid to.', annotations: [gap.id] });
      return;
    }
    say({ text: 'The page doesn\'t tell me what happens to state when the host dies, so I would close the tab before the license.' });
  }

  // The site view over the sample site map: this page's readings and the other pages'.
  let others = read ? siteReadings(now) : [];
  function siteState(over = {}) {
    const audiences = state.settings.audiences.filter((a) => a.source === 'site').map((a) => ({
      ...a, items: [...new Map([...(state.readings.find((r) => r.audienceID === a.id)?.items ?? []), ...others.flatMap((r) => r.items)]
        .filter((x) => x.needID).map((x) => [x.needID, { id: x.needID.split('.').slice(1).join('.'), field: x.field, text: x.text }])).values()],
    }));
    const readings = [...state.readings.map((r) => ({ ...r, page: state.page.url })), ...others];
    const grid = siteGrid({ map: SITE_MAP, audiences, readings, plans: state.plan ? [{ page: state.page.url, ...state.plan }] : [], here: state.page.url });
    return { ...state.site, ...grid, ...over };
  }
  if (site) state.site = siteState({ state: 'idle' });

  const handlers = {
    async 'site.read'({ urls } = {}) {
      const pages = urls?.length ? urls : SITE_MAP.map((p) => p.url);
      const total = pages.length;
      const started = Date.now();
      set({ site: siteState({ state: 'reading', progress: { done: 0, failed: 0, total }, running: pages.slice(0, 3), started }) });
      for (let i = 0; i < total; i++) {
        await wait(400);
        if (state.site.state !== 'reading') return;
        const url = pages[i];
        if (url !== state.page.url) {
          others = [...others.filter((r) => r.page !== url), { page: url, audienceID: 'lead', state: 'done', at: Date.now(), items: [
            { field: 'wants', text: 'tech-lead.decide', needID: 'tech-lead.decide', status: i % 2 ? 'answered' : 'partial', explanation: 'Read just now.' }] }];
        }
        set({ site: siteState({ state: 'reading', progress: { done: i + 1, failed: 0, total }, running: pages.slice(i + 1, i + 4), started }) });
      }
      set({ site: siteState({ state: 'done', running: [], at: Date.now() }) });
    },
    async 'site.cancel'() {
      if (state.site.state !== 'reading') throw new Error('The site is not being read.');
      set({ site: siteState({ state: 'cancelled', running: [], at: Date.now() }) });
    },
    async 'site.open'() {},
    async verify() {
      if (!state.readings.length) throw new Error('Read the page as an audience first.');
      set({ site: siteState({ state: 'reading', progress: { done: 0, failed: 0, total: state.readings.length + 1 }, running: [state.page.url], started: Date.now() }) });
      await wait(readingMS);
      const rank = { missing: 0, partial: 1, answered: 2 };
      const readings = state.readings.map((r) => {
        const after = r.items.map((x, i) => ({ ...x, status: i === 0 ? 'answered' : i === 1 ? 'partial' : i === 5 ? 'answered' : x.status }));
        const changes = after.map((x, i) => ({ needID: x.needID, text: x.text, from: r.items[i].status, to: x.status }));
        const count = (list) => list.filter((x) => x.status === 'answered').length;
        return { ...r, items: after, at: Date.now(), history: [{ at: r.at, answered: count(r.items), total: r.items.length }, ...(r.history ?? [])], compared: {
          before: { at: r.at, answered: count(r.items), total: r.items.length }, after: { answered: count(after), total: after.length },
          improved: changes.filter((c) => rank[c.to] > rank[c.from]), regressed: changes.filter((c) => rank[c.to] < rank[c.from]),
          unchanged: changes.filter((c) => c.to === c.from).length,
        } };
      });
      const criteria = state.criteria && { at: Date.now(), items: state.criteria.items.map((c, i) => ({ ...c, met: i === 0, ...(i === 0 ? { quote: 'What happens when the host dies? See Snapshots.' } : {}) })) };
      set({ readings, criteria, page: { ...state.page, changed: false } });
      set({ site: siteState({ state: 'done', running: [], at: Date.now() }) });
    },
    async annotate({ note }) {
      if (!state.selection) throw new Error('Nothing is selected on the page.');
      if (state.selection.fits === false) throw new Error('Select one sentence, heading or paragraph.');
      const t = Date.now();
      const annotation = {
        id: id('a'), state: 'draft', anchorFound: true, original: state.selection.text, note: note ?? '',
        suggestions: [], chosenID: null, messages: [], createdAt: t, updatedAt: t,
      };
      set({ annotations: [annotation, ...state.annotations], activeID: annotation.id, selection: null, picking: false });
      if (note) await ask(annotation.id, note);
      return annotation.id;
    },
    async select({ id: annotationID }) { set({ activeID: annotationID }); },
    async deselect() { set({ activeID: null }); },
    async ask({ id: annotationID, text }) { await ask(annotationID, text); },
    async suggest({ id: annotationID }) { await ask(annotationID, null); },
    async edit({ id: annotationID, text }) {
      const s = { id: id('s'), text, rationale: '', own: true, after: get(annotationID).messages.length };
      update(annotationID, (a) => ({ ...a, suggestions: [...a.suggestions, s], chosenID: s.id, state: a.anchorFound ? 'previewing' : 'suggested' }));
    },
    async preview({ id: annotationID, suggestionID }) {
      await wait(120);
      update(annotationID, (a) => ({ ...a, chosenID: suggestionID, state: 'previewing' }));
    },
    async keep({ id: annotationID }) { update(annotationID, (a) => ({ ...a, state: 'kept' })); },
    async undo({ id: annotationID }) { update(annotationID, (a) => ({ ...a, state: a.suggestions.length ? 'suggested' : 'draft', chosenID: null })); },
    async dismiss({ id: annotationID }) { update(annotationID, (a) => ({ ...a, state: 'dismissed', chosenID: null })); },
    async reopen({ id: annotationID }) { update(annotationID, (a) => ({ ...a, state: a.suggestions.length ? 'suggested' : 'draft' })); },
    async delete({ id: annotationID }) {
      set({ annotations: state.annotations.filter((a) => a.id !== annotationID), activeID: state.activeID === annotationID ? null : state.activeID });
    },
    async note({ id: annotationID, text }) { update(annotationID, (a) => ({ ...a, note: text })); },
    async chat({ text, spoken }) {
      set({ chat: chatSet({ messages: [...state.chat.messages, { role: 'user', text, at: Date.now(), ...(spoken ? { spoken: true } : {}) }], busy: true }) });
      await wait(1100);
      if (state.chat.audienceID) { await readerReply(text); return; }
      if (/talk to|let me talk/i.test(text)) {
        const lead = state.settings.audiences.find((a) => a.id === 'lead') ?? state.settings.audiences[0];
        const talkID = id('c');
        set({ chat: chatSet({ busy: false, messages: [...state.chat.messages, {
          role: 'model', at: Date.now(), text: `Here is ${lead.name}.`, audiences: [{ name: lead.name, action: 'talk', id: lead.id, chat: talkID }],
        }] }) });
        whose.set(talkID, lead.id);
        set({ chat: chatSet({ id: talkID, audienceID: lead.id, messages: [], busy: false }) });
        return;
      }
      if (/search/i.test(text)) {
        set({ chat: chatSet({ busy: false, messages: [...state.chat.messages, {
          role: 'model', at: Date.now(), text: 'Akka calls itself a platform for agentic AI; Orleans a framework for distributed apps.',
          sources: [{ title: 'akka.io', uri: 'https://akka.io/' }, { title: 'learn.microsoft.com', uri: 'https://learn.microsoft.com/dotnet/orleans/' }],
        }] }) });
        return;
      }
      if (/how would|read as/i.test(text)) {
        const lead = state.settings.audiences.find((a) => a.id === 'lead') ?? state.settings.audiences[0];
        set({ chat: chatSet({ busy: false, messages: [...state.chat.messages, {
          role: 'model', at: Date.now(), text: `Reading the page as ${lead.name}: the findings come into the list.`,
          audiences: [{ name: lead.name, action: 'read', id: lead.id }],
        }] }) });
        await handlers.read({ audienceIDs: [lead.id] });
        return;
      }
      if (/who|audience|persona/i.test(text)) {
        const audience = {
          id: id('u'), name: 'Skeptical backend engineer', description: 'A senior backend engineer evaluating actor runtimes.',
          knows: 'Threads, queues, Erlang.', wants: 'Numbers: memory per actor.', tasks: 'Decide whether to try it.', objections: 'Hype.', source: 'model',
        };
        set({
          settings: { ...state.settings, audiences: [...state.settings.audiences, audience], audienceIDs: [...state.settings.audienceIDs, audience.id] },
          chat: chatSet({
            busy: false,
            messages: [...state.chat.messages, {
              role: 'model', at: Date.now(), text: 'The page is for backend engineers evaluating actor runtimes; I saved them as an audience.',
              audiences: [{ name: audience.name, action: 'define', id: audience.id }], thoughts: '**Finding the reader**\nThe page talks to engineers.', seconds: 4,
            }],
          }),
        });
        return;
      }
      const t = Date.now();
      const proposed = {
        id: id('a'), state: 'suggested', anchorFound: true, original: 'Everything you need to build actor systems',
        note: '',
        source: 'model',
        suggestions: [{ id: id('s'), text: 'Spawn, send and supervise actors', rationale: 'Verbs first; no "everything".', after: 0 }],
        chosenID: null, messages: [], createdAt: t, updatedAt: t,
      };
      set({
        annotations: [proposed, ...state.annotations],
        chat: chatSet({
          busy: false,
          messages: [...state.chat.messages, {
            role: 'model', at: t, annotations: [proposed.id],
            text: 'The page has **three headings** over 8 words. The second says least: lead with the verbs.',
          }],
        }),
      });
    },
    async 'chat.new'() {
      if (!state.chat.messages.length && !state.chat.audienceID) return;
      set({ chat: chatSet({ id: id('c'), audienceID: null, messages: [], busy: false }) });
    },
    async 'chat.open'({ id: chatID }) {
      if (!conversations.has(chatID) && !whose.has(chatID)) throw new Error('No such conversation on this page.');
      set({ chat: chatSet({ id: chatID, audienceID: whose.get(chatID) ?? null, messages: conversations.get(chatID) ?? [], busy: false }) });
    },
    async talk({ audienceID }) {
      if (!state.settings.audiences.some((a) => a.id === audienceID)) throw new Error('No such audience.');
      if (!state.chat.messages.length && state.chat.audienceID === audienceID) return state.chat.id;
      const talkID = id('c');
      set({ chat: chatSet({ id: talkID, audienceID, messages: [], busy: false }) });
      return talkID;
    },
    async read({ audienceIDs }) {
      const ids = audienceIDs ?? state.settings.audienceIDs;
      if (!ids.length) throw new Error('Choose an audience first.');
      const others = state.readings.filter((r) => !ids.includes(r.audienceID));
      set({ readings: [...others, ...ids.map((audienceID) => ({ ...(state.readings.find((r) => r.audienceID === audienceID) ?? { items: [], findingIDs: [] }), audienceID, state: 'reading', at: Date.now() }))] });
      await wait(readingMS);
      const fresh = ids.map((audienceID) => {
        const fid = id('f');
        return {
          reading: {
            audienceID, state: 'done', at: Date.now(), seconds: 2, findingIDs: [fid],
            items: [
              { field: 'wants', text: 'One small example', status: 'partial', explanation: 'The example needs its imports.' },
              { field: 'objections', text: '"Is it maintained?"', status: 'missing', explanation: 'No maintainer is named.' },
            ],
          },
          finding: {
            id: fid, state: 'draft', anchorFound: true, source: 'audience', original: 'Statecharts run the same everywhere.', note: '', chosenID: null,
            messages: [], suggestions: [], audiences: [], createdAt: Date.now(), updatedAt: Date.now(),
            finding: { audienceID, kind: 'jargon', explanation: 'What is a statechart?' },
          },
        };
      });
      set({
        readings: [...state.readings.filter((r) => !ids.includes(r.audienceID)), ...fresh.map((f) => f.reading)],
        annotations: [...fresh.map((f) => f.finding), ...state.annotations],
      });
    },
    async 'reading.show'({ audienceID, index }) {
      const item = state.readings.find((r) => r.audienceID === audienceID)?.items[index];
      if (!item?.anchor) throw new Error('The page has no text for it.');
    },
    async 'reading.open'({ audienceID, index }) {
      const r = state.readings.find((x) => x.audienceID === audienceID);
      const item = r.items[index];
      const t = Date.now();
      const annotation = {
        id: id('g'), state: 'draft', anchorFound: true, pageLevel: true, source: 'audience', original: item.text, note: '', chosenID: null,
        messages: [], suggestions: [], audiences: [], createdAt: t, updatedAt: t,
        finding: { audienceID, kind: 'missing', explanation: item.explanation, item: { field: item.field, status: item.status } },
      };
      set({
        annotations: [annotation, ...state.annotations],
        readings: state.readings.map((x) => (x === r ? { ...r, items: r.items.map((y, i) => (i === index ? { ...y, annotationID: annotation.id } : y)) } : x)),
      });
      return annotation.id;
    },
    async 'pick.start'() { set({ picking: true }); },
    async 'pick.stop'() { set({ picking: false }); },
        async 'record.start'({ target }) { set({ recording: { state: 'recording', target } }); },
    async 'record.stop'({ target }) {
      set({ recording: { state: 'transcribing', target } });
      const annotation = target && target !== 'page' && get(target);
      if (annotation) update(target, (a) => ({ ...a, state: 'transcribing' }));
      else set({ chat: chatSet({ busy: true }) });
      await wait(700);
      set({ recording: { state: 'idle', target: null } });
      const text = 'Make it shorter and lead with the number.';
      if (annotation) {
        update(target, (a) => ({ ...a, messages: [...a.messages, { role: 'user', text, at: Date.now(), spoken: true }] }));
        await ask(target, null);
      } else {
        await handlers.chat({ text, spoken: true });
      }
    },
    async 'settings.save'(command) {
      const settings = { ...state.settings };
      if (command.apiKey && /bad/i.test(command.apiKey)) { await wait(150); throw new Error('That key was not accepted by Google.'); }
      if (command.apiKey) settings.hasKey = true;
      if (command.rememberKey != null) settings.rememberKey = command.rememberKey;
      if (command.model) settings.model = command.model;
      if (command.thinking) settings.thinking = command.thinking;
      if (command.length) settings.length = command.length;
      if (command.tools) settings.tools = { ...settings.tools, ...command.tools };
      if (command.voice !== undefined) settings.voice = command.voice ? { instructions: command.voice, custom: command.voice !== VOICE } : { instructions: VOICE, custom: false };
      if (command.audienceIDs) {
        settings.audienceIDs = command.audienceIDs;
        state = Object.freeze({ ...state, page: { ...state.page, audienceSuggestion: null } });  // the page's set now
      }
      if (command.audience) {
        const audience = { ...command.audience, id: command.audience.id || id('u') };
        settings.audiences = settings.audiences.some((a) => a.id === audience.id)
          ? settings.audiences.map((a) => (a.id === audience.id ? { ...a, ...audience } : a))
          : [...settings.audiences, audience];
      }
      await wait(150);
      set({ settings });
    },
    async 'audiences.accept'() {
      const sug = state.page.audienceSuggestion;
      if (sug?.state !== 'ready') throw new Error('No suggestion to accept.');
      await wait(100);
      set({ page: { ...state.page, audienceSuggestion: null }, settings: { ...state.settings, audienceIDs: [...sug.audienceIDs] } });
    },
    async 'audiences.dismiss'() {
      if (state.page.audienceSuggestion?.state !== 'ready') throw new Error('No suggestion to dismiss.');
      set({ page: { ...state.page, audienceSuggestion: null } });
    },
    async 'settings.deleteAudience'({ id: audienceID }) {
      set({ settings: {
        ...state.settings,
        audiences: state.settings.audiences.filter((a) => a.id !== audienceID),
        audienceIDs: state.settings.audienceIDs.filter((x) => x !== audienceID),
      } });
    },
    async 'settings.forgetKey'() { set({ settings: { ...state.settings, hasKey: false, rememberKey: false } }); },
    async 'models.refresh'() {
      await wait(700);
      const models = state.settings.models.some((m) => m.id === 'gemini-4-hydrogen-lite')
        ? state.settings.models
        : [...state.settings.models, { id: 'gemini-4-hydrogen-lite', label: 'Gemini 4 Hydrogen Lite' }];
      set({ settings: { ...state.settings, models } });
    },
    async plan() {
      if (!state.settings.hasKey) throw new Error('No Gemini key: add yours in Settings.');
      const before = state.plan;
      set({ plan: { ...(before ?? { items: [], newPages: [], sitePages: 22 }), state: 'planning', at: Date.now() } });
      await wait(900);
      const fresh = samplePlan();
      const kept = (list) => (list ?? []).filter((x) => x.decision);
      const same = (x, y) => x.action === y.action && x.target === y.target && (x.original ?? '') === (y.original ?? '');
      set({ plan: {
        state: 'planned', at: Date.now(), seconds: 31, sitePages: 22,
        items: [...kept(before?.items), ...fresh.items.filter((x) => !kept(before?.items).some((y) => same(x, y)))],
        newPages: [...kept(before?.newPages), ...fresh.newPages.filter((x) => !kept(before?.newPages).some((y) => y.title === x.title))],
      } });
    },
    async 'plan.accept'({ id: itemID }) { decide([itemID], 'accepted'); },
    async 'plan.reject'({ id: itemID }) { decide([itemID], 'rejected'); },
    async 'plan.acceptAll'() { decide([...state.plan.items, ...state.plan.newPages].filter((x) => !x.decision).map((x) => x.id), 'accepted'); },
    async export({ scope = 'page' } = {}) {
      set({ exporting: 'building' });
      await wait(600);
      if (state.totals.kept === 0) {
        set({ exporting: 'failed' });
        throw new Error('Nothing is kept yet: keep a suggestion first.');
      }
      // the real packet over the mock's records: the page's annotations and its accepted plan
      const url = state.page.url;
      const iso = (t) => new Date(t).toISOString();
      const records = state.annotations.map((a) => ({
        ...a, page: url, lang: 'en', anchor: a.pageLevel ? null : { quote: { exact: a.original, prefix: '', suffix: '' }, selector: 'main p' },
        createdAt: iso(a.createdAt), updatedAt: iso(a.updatedAt),
      }));
      const accepted = (list) => (list ?? []).filter((x) => x.decision === 'accepted');
      const plans = state.plan ? [{ page: url, items: accepted(state.plan.items).map((x) => ({ ...x, original_text: x.original, target_text: x.targetText })), newPages: accepted(state.plan.newPages) }] : [];
      exported = (await buildPacket({ annotations: records, plans, pages: { [url]: { title: state.page.title, lang: 'en', kind: state.page.kind } }, title: `Copy review (${scope})` })).changes;
      set({ exporting: 'done' });
      setTimeout(() => set({ exporting: 'idle' }), 1500 * delay);
    },
  };

  state.totals = totals(state.annotations);
  state = Object.freeze(state);

  return {
    getState: () => state,
    subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    // settles when the command's work is done; rejects with a message for the user
    dispatch(command) {
      log.push(command);
      const handler = handlers[command?.type];
      if (!handler) return Promise.reject(new Error(`unknown command ${command?.type}`));
      return handler(command);
    },
    ready: () => (state.status === 'error' ? Promise.reject(new Error(state.error)) : Promise.resolve()),
    close() { listeners.clear(); },
    // for the preview and tests only: what the page would report, what was dispatched
    setSelection(text, { fits = true } = {}) { set({ selection: text ? { text, fits } : null }); },
    setReadingTime(ms) { readingMS = ms; },
    exported: () => exported,
    set,
    log,
  };
}
