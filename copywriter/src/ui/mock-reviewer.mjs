// A fake Reviewer for developing and testing the panel without the actor
// host or Gemini: the facade of AGENTS.md ("The facade: Reviewer") over a
// plain object, with sample state for www.tinyactors.dev and simulated
// delays for the model.
//
//   const reviewer = createMockReviewer({ hasKey: true, delay: 1 });
//   reviewer.setSelection('Actors as a library');   // what the page would report
//
// `delay` scales every simulated wait (0 makes them immediate macrotasks).

const MINUTE = 60_000;

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

export function createMockReviewer({ hasKey = true, delay = 1, empty = false, status = 'ready' } = {}) {
  const now = Date.now();
  let seq = 100;
  const listeners = new Set();
  const log = [];
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms * delay));
  const id = (prefix) => `${prefix}${++seq}`;

  let state = {
    status,
    page: { url: 'https://www.tinyactors.dev/', title: 'Actors as a library' },
    selection: null,
    settings: {
      hasKey, rememberKey: false, model: 'gemini-4-hydrogen', thinking: 'high',
      models: [
        { id: 'gemini-4-hydrogen', label: 'Gemini 4 Hydrogen' },
        { id: 'gemini-3.8-flash', label: 'Gemini 3.8 Flash' },
        { id: 'gemini-3.5-pro', label: 'Gemini 3.5 Pro' },
      ],
      personaID: 'voice',
      personas: [
        { id: 'voice', name: 'tinyactors.dev voice', instructions: VOICE, builtIn: true },
        { id: 'plain', name: 'Plain English', instructions: 'Short words. One idea per sentence.' },
      ],
    },
    annotations: empty ? [] : sample(now),
    activeID: null,
    chat: { messages: [], busy: false },
    recording: { state: 'idle', target: null },
    totals: { kept: 0, open: 0, pages: 3 },
    exporting: 'idle',
  };

  function totals(annotations) {
    const kept = annotations.filter((a) => a.state === 'kept').length;
    const open = annotations.filter((a) => !['kept', 'dismissed'].includes(a.state)).length;
    return { kept: kept + 2, open, pages: state.totals.pages };   // two kept on other pages
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

  const handlers = {
    async annotate({ note }) {
      if (!state.selection) throw new Error('Nothing is selected on the page.');
      if (state.selection.fits === false) throw new Error('Select one sentence, heading or paragraph.');
      const t = Date.now();
      const annotation = {
        id: id('a'), state: 'draft', anchorFound: true, original: state.selection.text, note: note ?? '',
        suggestions: [], chosenID: null, messages: [], createdAt: t, updatedAt: t,
      };
      set({ annotations: [annotation, ...state.annotations], activeID: annotation.id, selection: null });
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
      set({ chat: { messages: [...state.chat.messages, { role: 'user', text, at: Date.now(), ...(spoken ? { spoken: true } : {}) }], busy: true } });
      await wait(1100);
      if (/persona/i.test(text)) {
        const persona = { id: id('p'), name: 'Skeptical backend engineer', instructions: 'Readers are senior backend engineers. Prove every claim.', source: 'model' };
        set({
          settings: { ...state.settings, personas: [...state.settings.personas, persona], personaID: persona.id },
          chat: {
            busy: false,
            messages: [...state.chat.messages, {
              role: 'model', at: Date.now(), text: 'Saved a persona for senior backend engineers and switched to it.',
              personas: [{ name: persona.name, action: 'define', id: persona.id }], thoughts: '**Defining the reader**\nSenior engineers distrust adjectives.', seconds: 4,
            }],
          },
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
        chat: {
          busy: false,
          messages: [...state.chat.messages, {
            role: 'model', at: t, annotations: [proposed.id],
            text: 'The page has **three headings** over 8 words. The second says least: lead with the verbs.',
          }],
        },
      });
    },
    async 'record.start'({ target }) { set({ recording: { state: 'recording', target } }); },
    async 'record.stop'({ target }) {
      set({ recording: { state: 'transcribing', target } });
      const annotation = target && target !== 'page' && get(target);
      if (annotation) update(target, (a) => ({ ...a, state: 'transcribing' }));
      else set({ chat: { ...state.chat, busy: true } });
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
      if (command.personaID) settings.personaID = command.personaID;
      if (command.thinking) settings.thinking = command.thinking;
      if (command.persona) settings.personas = settings.personas.map((p) => (p.id === command.persona.id ? { ...p, ...command.persona } : p));
      await wait(150);
      set({ settings });
    },
    async 'settings.deletePersona'({ id: personaID }) {
      const settings = { ...state.settings, personas: state.settings.personas.filter((p) => p.id !== personaID) };
      if (settings.personaID === personaID) settings.personaID = settings.personas[0].id;
      set({ settings });
    },
    async 'settings.forgetKey'() { set({ settings: { ...state.settings, hasKey: false, rememberKey: false } }); },
    async 'models.refresh'() {
      await wait(700);
      const models = state.settings.models.some((m) => m.id === 'gemini-4-hydrogen-lite')
        ? state.settings.models
        : [...state.settings.models, { id: 'gemini-4-hydrogen-lite', label: 'Gemini 4 Hydrogen Lite' }];
      set({ settings: { ...state.settings, models } });
    },
    async export() {
      set({ exporting: 'building' });
      await wait(600);
      if (state.totals.kept === 0) {
        set({ exporting: 'failed' });
        throw new Error('Nothing is kept yet: keep a suggestion first.');
      }
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
    set,
    log,
  };
}
