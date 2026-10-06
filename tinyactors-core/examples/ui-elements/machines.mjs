/**
 * The machines behind the custom elements (no DOM here: the elements render
 * snapshots and send events; tests drive the same machines directly).
 * Pass the API of a binding; `t` scales the timings (tests use small ones).
 */
export function uiMachines({ createMachine, assign, sendTo, raise }, t = {}) {
  const ms = { repeatDelay: 400, repeatEvery: 70, debounce: 250, latency: 300, dialog: 180, submit: 600, ...t };

  /* <ta-toggle> — on/off, can be disabled */
  const toggle = createMachine({
    id: 'toggle',
    context: ({ input }) => ({ disabled: !!input?.disabled }),
    initial: 'off',
    on: { DISABLE: { actions: assign({ disabled: true }) }, ENABLE: { actions: assign({ disabled: false }) } },
    states: {
      off: { on: { TOGGLE: { guard: ({ context }) => !context.disabled, target: 'on' } } },
      on: { on: { TOGGLE: { guard: ({ context }) => !context.disabled, target: 'off' } } },
    },
  });

  /* <ta-counter> — bounded, auto-repeats while a button is held */
  const step = (dir) =>
    assign(({ context }) => ({ value: Math.min(context.max, Math.max(context.min, context.value + dir * context.step)) }));
  const counter = createMachine(
    {
      id: 'counter',
      context: ({ input = {} }) => ({ value: input.value ?? 0, min: input.min ?? -Infinity, max: input.max ?? Infinity, step: input.step ?? 1, dir: 1 }),
      initial: 'idle',
      states: {
        idle: {
          on: {
            INC: { actions: step(1) },
            DEC: { actions: step(-1) },
            PRESS: { target: 'held', actions: [assign({ dir: ({ event }) => event.dir }), 'stepDir'] },
          },
        },
        held: {
          on: { RELEASE: 'idle' },
          after: { repeatDelay: 'repeating' },
        },
        repeating: {
          on: { RELEASE: 'idle' },
          after: { repeatEvery: { target: 'repeating', reenter: true, actions: 'stepDir' } },
          always: { guard: 'atBound', target: 'idle' },
        },
      },
    },
    {
      actions: { stepDir: assign(({ context }) => ({ value: Math.min(context.max, Math.max(context.min, context.value + context.dir * context.step)) })) },
      guards: { atBound: ({ context }) => (context.dir > 0 ? context.value >= context.max : context.value <= context.min) },
      delays: { repeatDelay: ms.repeatDelay, repeatEvery: ms.repeatEvery },
    },
  );

  /* <ta-search> — debounced autocomplete over a slow "server"; stale answers are
   * dropped (each request has an id), arrow keys move, Enter picks */
  const search = createMachine(
    {
      id: 'search',
      context: ({ input = {} }) => ({ words: input.words || [], query: '', results: [], active: -1, picked: null, request: 0 }),
      initial: 'idle',
      on: {
        TYPE: { target: '.debouncing', reenter: true, actions: assign({ query: ({ event }) => event.value, active: -1 }) },
        CLEAR: { target: '.idle', actions: assign({ query: '', results: [], active: -1 }) },
      },
      states: {
        idle: {},
        debouncing: {
          after: { debounce: [{ guard: ({ context }) => !context.query.trim(), target: 'idle', actions: assign({ results: [] }) }, { target: 'searching' }] },
        },
        searching: {
          entry: [
            assign({ request: ({ context }) => context.request + 1 }),
            raise(({ context }) => ({ type: 'RESULTS', request: context.request, results: context.words.filter((w) => w.toLowerCase().includes(context.query.toLowerCase())).slice(0, 8) }), { delay: 'latency' }),
          ],
          on: {
            RESULTS: [
              { guard: ({ context, event }) => event.request === context.request, target: 'open', actions: assign({ results: ({ event }) => event.results }) },
              {}, /* a stale answer: ignore */
            ],
          },
        },
        open: {
          on: {
            DOWN: { actions: assign({ active: ({ context }) => Math.min(context.results.length - 1, context.active + 1) }) },
            UP: { actions: assign({ active: ({ context }) => Math.max(-1, context.active - 1) }) },
            PICK: {
              guard: ({ context, event }) => (event.index ?? context.active) >= 0 && (event.index ?? context.active) < context.results.length,
              target: 'idle',
              actions: [
                assign({ picked: ({ context, event }) => context.results[event.index ?? context.active] }),
                assign({ query: ({ context }) => context.picked, results: [], active: -1 }),
              ],
            },
            ESCAPE: { target: 'idle', actions: assign({ results: [], active: -1 }) },
          },
        },
      },
    },
    { delays: { debounce: ms.debounce, latency: ms.latency } },
  );

  /* <ta-field> — validates itself and tells its form (actor to actor) */
  const field = createMachine({
    id: 'field',
    context: ({ input }) => ({ name: input.name, rule: input.rule, value: '', error: null, form: input.form }),
    initial: 'pristine',
    states: {
      pristine: { on: { INPUT: { target: 'editing', actions: ['update', 'report'] } } },
      editing: {
        on: { INPUT: { actions: ['update', 'report'] }, BLUR: 'checked' },
      },
      checked: { on: { INPUT: { actions: ['update', 'report'] } } },
    },
  }, {
    actions: {
      update: assign(({ context, event }) => ({ value: event.value, error: validate(context.rule, event.value) })),
      report: sendTo(({ context }) => context.form, ({ context }) => ({ type: 'FIELD', name: context.name, valid: !context.error, value: context.value })),
    },
  });

  /* <ta-form> — enabled when every field is valid; submitting takes a while */
  const form = createMachine(
    {
      id: 'form',
      context: ({ input }) => ({ fields: Object.fromEntries((input.fields || []).map((n) => [n, { valid: false, value: '' }])), sent: null }),
      initial: 'editing',
      states: {
        editing: {
          on: {
            FIELD: { actions: assign(({ context, event }) => ({ fields: { ...context.fields, [event.name]: { valid: event.valid, value: event.value } } })) },
            SUBMIT: { guard: 'allValid', target: 'submitting' },
          },
        },
        submitting: {
          after: { submit: { target: 'done', actions: assign(({ context }) => ({ sent: Object.fromEntries(Object.entries(context.fields).map(([k, f]) => [k, f.value])) })) } },
        },
        done: { on: { RESET: { target: 'editing' } } },
      },
    },
    { guards: { allValid: ({ context }) => Object.values(context.fields).every((f) => f.valid) }, delays: { submit: ms.submit } },
  );

  /* <ta-dialog> — opening and closing take time (the element animates them) */
  const dialog = createMachine(
    {
      id: 'dialog',
      initial: 'closed',
      states: {
        closed: { on: { OPEN: 'opening' } },
        opening: { after: { anim: 'open' }, on: { CLOSE: 'closing' } },
        open: { on: { CLOSE: 'closing', ESCAPE: 'closing' } },
        closing: { after: { anim: 'closed' }, on: { OPEN: 'opening' } },
      },
    },
    { delays: { anim: ms.dialog } },
  );

  return { toggle, counter, search, field, form, dialog };
}

export function validate(rule, value) {
  const v = String(value || '').trim();
  if (rule === 'name') return v.length >= 2 ? null : 'at least 2 characters';
  if (rule === 'email') return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v) ? null : 'not an email address';
  if (rule === 'age') return /^\d+$/.test(v) && Number(v) >= 13 && Number(v) <= 130 ? null : '13 to 130';
  return null;
}
