/**
 * The charts behind the custom elements (no DOM here: the elements render
 * snapshots and send events; tests drive the same charts directly).
 * Pass the API of a binding; `t` scales the timings (tests use small ones).
 */
export function uiCharts({ scxml }, t = {}) {
  const ms = { repeatDelay: 400, repeatEvery: 70, debounce: 250, latency: 300, dialog: 180, submit: 600, ...t };
  const c = scxml.chart();
  const after = (event, delay) => [c.onentry(c.send(event, { delay: `${delay}ms`, id: event })), c.onexit(c.cancel(event))];

  /* <ta-toggle> — on/off, can be disabled (input: disabled) */
  const disabling = [c.transition('DISABLE', undefined, c.assign('disabled', 'true')), c.transition('ENABLE', undefined, c.assign('disabled', 'false'))];
  const toggle = c.scxml('toggle', { initial: 'off', datamodel: 'ecmascript' },
    c.datamodel({ disabled: false }),
    c.state('off', c.transition('TOGGLE', 'on', { cond: '!disabled' }), disabling),
    c.state('on', c.transition('TOGGLE', 'off', { cond: '!disabled' }), disabling));

  /* <ta-counter> — bounded, auto-repeats while a button is held (input: value, min, max, step) */
  const step = (dir) => c.assign('value', c.expr(({ value, min, max, step: by }) => Math.min(max, Math.max(min, value + dir * by))));
  const stepDir = c.assign('value', c.expr(({ value, min, max, step: by, dir }) => Math.min(max, Math.max(min, value + dir * by))));
  const counter = c.scxml('counter', { initial: 'idle', datamodel: 'ecmascript' },
    c.datamodel({ value: 0, min: '-Infinity', max: 'Infinity', step: 1, dir: 1 }),
    c.state('idle',
      c.transition('INC', undefined, step(1)),
      c.transition('DEC', undefined, step(-1)),
      c.transition('PRESS', 'held', c.assign('dir', '_event.data.dir'), stepDir)),
    c.state('held',
      after('repeatDelay', ms.repeatDelay),
      c.transition('RELEASE', 'idle'),
      c.transition('repeatDelay', 'repeating')),
    c.state('repeating',
      after('repeatEvery', ms.repeatEvery),
      c.transition('RELEASE', 'idle'),
      c.transition('repeatEvery', 'repeating', stepDir),
      c.transition(undefined, 'idle', { cond: c.expr(({ dir, value, min, max }) => (dir > 0 ? value >= max : value <= min)) })));

  /* <ta-search> — debounced autocomplete over a slow "server"; stale answers are
   * dropped (each request has a number), arrow keys move, Enter picks (input: words) */
  const typing = [
    c.transition('TYPE', 'debouncing', c.assign('query', '_event.data.value'), c.assign('active', '-1')),
    c.transition('CLEAR', 'idle', c.assign('query', '""'), c.assign('results', '[]'), c.assign('active', '-1')),
  ];
  const search = c.scxml('search', { initial: 'idle', datamodel: 'ecmascript' },
    c.datamodel({ words: [], query: '""', results: [], active: -1, picked: null, request: 0 }),
    c.state('idle', typing),
    c.state('debouncing',
      after('debounce', ms.debounce),
      typing,
      c.transition('debounce', 'idle', { cond: '!query.trim()' }, c.assign('results', '[]')),
      c.transition('debounce', 'searching')),
    c.state('searching',
      c.onentry(
        c.assign('request', 'request + 1'),
        c.send('RESULTS', { delay: `${ms.latency}ms` },
          c.content(c.expr(({ request, words, query }) => ({ request, results: words.filter((w) => w.toLowerCase().includes(query.toLowerCase())).slice(0, 8) }))))),
      typing,
      /* an answer to an earlier request is stale: no transition takes it */
      c.transition('RESULTS', 'open', { cond: '_event.data.request === request' }, c.assign('results', '_event.data.results'))),
    c.state('open',
      typing,
      c.transition('DOWN', undefined, c.assign('active', 'Math.min(results.length - 1, active + 1)')),
      c.transition('UP', undefined, c.assign('active', 'Math.max(-1, active - 1)')),
      c.transition('PICK', 'idle', { cond: c.expr(({ results, active, _event }) => (_event.data.index ?? active) >= 0 && (_event.data.index ?? active) < results.length) },
        c.assign('picked', c.expr(({ results, active, _event }) => results[_event.data.index ?? active])),
        c.assign('query', 'picked'), c.assign('results', '[]'), c.assign('active', '-1')),
      c.transition('ESCAPE', 'idle', c.assign('results', '[]'), c.assign('active', '-1'))));

  /* <ta-field> — validates itself and tells its form, actor to actor (input: name, rule, form) */
  const update = [
    c.assign('value', '_event.data.value'),
    c.assign('error', c.expr(({ rule, value }) => validate(rule, value))),
    c.send('FIELD', { targetexpr: 'String(form)' }, c.content(c.expr(({ name, error, value }) => ({ name, valid: !error, value })))),
  ];
  const field = c.scxml('field', { initial: 'pristine', datamodel: 'ecmascript' },
    c.datamodel({ name: '""', rule: '""', value: '""', error: null, form: null }),
    c.state('pristine', c.transition('INPUT', 'editing', update)),
    c.state('editing', c.transition('INPUT', undefined, update), c.transition('BLUR', 'checked')),
    c.state('checked', c.transition('INPUT', undefined, update)));

  /* <ta-form> — enabled when every field is valid; submitting takes a while (input: names) */
  const form = c.scxml('form', { initial: 'editing', datamodel: 'ecmascript' },
    c.datamodel({ names: [], fields: c.expr(({ names }) => Object.fromEntries(names.map((n) => [n, { valid: false, value: '' }]))), sent: null }),
    c.state('editing',
      c.transition('FIELD', undefined, c.assign('fields', c.expr(({ fields, _event: { data } }) => ({ ...fields, [data.name]: { valid: data.valid, value: data.value } })))),
      c.transition('SUBMIT', 'submitting', { cond: c.expr(({ fields }) => Object.values(fields).every((f) => f.valid)) })),
    c.state('submitting',
      after('submitted', ms.submit),
      c.transition('submitted', 'done', c.assign('sent', c.expr(({ fields }) => Object.fromEntries(Object.entries(fields).map(([k, f]) => [k, f.value])))))),
    c.state('done', c.transition('RESET', 'editing')));

  /* <ta-dialog> — opening and closing take time (the element animates them) */
  const dialog = c.scxml('dialog', { initial: 'closed', datamodel: 'ecmascript' },
    c.state('closed', c.transition('OPEN', 'opening')),
    c.state('opening', after('opened', ms.dialog), c.transition('opened', 'open'), c.transition('CLOSE', 'closing')),
    c.state('open', c.transition('CLOSE', 'closing'), c.transition('ESCAPE', 'closing')),
    c.state('closing', after('closed', ms.dialog), c.transition('closed', 'closed'), c.transition('OPEN', 'opening')));

  return { toggle, counter, search, field, form, dialog };
}

export function validate(rule, value) {
  const v = String(value || '').trim();
  if (rule === 'name') return v.length >= 2 ? null : 'at least 2 characters';
  if (rule === 'email') return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v) ? null : 'not an email address';
  if (rule === 'age') return /^\d+$/.test(v) && Number(v) >= 13 && Number(v) <= 130 ? null : '13 to 130';
  return null;
}
