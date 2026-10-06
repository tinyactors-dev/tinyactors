/* Keymaps are data. Each trie node below becomes a nested SCXML state; the
 * interpreter, not a JS key dispatcher, owns prefixes and their deadlines. */
export const sampleText = `one two three four five
const greeting = "hello actors";
const answer = 42;

// Try 3dw, then u. Try i, type, Escape, then .
// A lone j waits. A quick jj leaves insert mode.
// Switch keymaps without replacing this actor.
`;

export function keyBindings(keymap, escape = 'j j') {
  const motions = { h: 'left', l: 'right', w: 'word', b: 'backWord', '0': 'lineStart', '$': 'lineEnd', 'g g': 'start', G: 'end', j: 'down', k: 'up' };
  if (keymap === 'emacs') return { emacs: {
    'C-f': 'right', 'C-b': 'left', 'M-f': 'word', 'M-b': 'backWord', 'C-n': 'down', 'C-p': 'up',
    'C-a': 'lineStart', 'C-e': 'lineEnd', 'M-<': 'start', 'M->': 'end',
    'C-x C-s': 'save', 'C-x u': 'undo', 'C-/': 'undo', 'C-x r r': 'redo',
    'C-Space': 'mark', 'C-w': 'cut', 'C-g': 'escape', 'C-d': 'delete', Backspace: 'backspace', Enter: 'newline',
  } };
  const normal = { ...motions, i: 'insert', a: 'append', v: 'visual', x: 'delete', u: 'undo', 'C-r': 'redo', '.': 'repeat', 'C-s': 'save' };
  for (const op of ['d', 'c']) {
    for (const [key, motion] of Object.entries(motions)) normal[`${op} ${key}`] = `${op}:${motion}`;
    normal[`${op} ${op}`] = `${op}:line`;
    normal[`${op} i "`] = `${op}:quote`;
  }
  for (const register of 'abcdefghijklmnopqrstuvwxyz') {
    normal[`q ${register}`] = `record:${register}`;
    normal[`@ ${register}`] = `play:${register}`;
  }
  return {
    normal,
    visual: { ...motions, d: 'cut', x: 'cut', c: 'changeSelection', v: 'escape', 'C-s': 'save' },
    insert: { [escape]: 'escape', Escape: 'escape', Backspace: 'backspace', Enter: 'newline', 'C-s': 'save', ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down' },
  };
}

/* Keys are SCXML event names, from general to specific: `key.char.letter.d`,
 * `key.char.digit.0`, `key.char.symbol.quote`, `key.ctrl.x`, `key.meta.less`,
 * `key.special.escape`. A transition on `stroke.char` takes every printable
 * key and one on `stroke.char.digit` every digit (an event descriptor matches
 * all names it is a prefix of, SCXML 3.12.1), so no guard inspects the key and
 * document order says which wins: `stroke.char.digit.0` before `stroke.char.digit`
 * before `stroke.char` before `stroke`. */
const SYMBOLS = {
  ' ': 'space', '.': 'period', ',': 'comma', '$': 'dollar', '@': 'at', '"': 'quote', "'": 'apostrophe', '<': 'less', '>': 'greater',
  '/': 'slash', '\\': 'backslash', '-': 'minus', '+': 'plus', '*': 'asterisk', '#': 'hash', '%': 'percent', '^': 'caret', '&': 'ampersand',
  '(': 'lparen', ')': 'rparen', '[': 'lbracket', ']': 'rbracket', '{': 'lbrace', '}': 'rbrace', '=': 'equals', '!': 'bang', '?': 'question',
  ':': 'colon', ';': 'semicolon', '_': 'underscore', '`': 'backtick', '~': 'tilde', '|': 'bar',
};
const token = (s) =>
  /^[A-Za-z0-9]$/.test(s) ? s : SYMBOLS[s] || (/^[A-Za-z0-9]+$/.test(s) ? s.toLowerCase() : Array.from(s, (ch) => `u${ch.codePointAt(0).toString(16)}`).join(''));
/** The event name of a key written as in the keymaps ('d', '0', '"', 'C-x', 'M-<', 'Escape'). */
export function keyName(key) {
  const chord = /^([CM])-(.+)$/su.exec(key);
  if (chord) return `key.${chord[1] === 'C' ? 'ctrl' : 'meta'}.${chord[2] === 'Space' ? 'space' : token(chord[2])}`;
  if (Array.from(key).length > 1) return `key.special.${token(key)}`;
  return `key.char.${/[0-9]/.test(key) ? 'digit' : /\p{L}/u.test(key) ? 'letter' : 'symbol'}.${token(key)}`;
}
/** The event an element sends for a key press. */
export const keyEvent = (key) => ({ type: keyName(key), key });
const stroke = (key) => `stroke.${keyName(key).slice('key.'.length)}`;
/** An `on` object from [event, transition] pairs: one array per event, in first-seen order. */
const on = (...pairs) => {
  const o = {};
  for (const [event, t] of pairs.flat()) (o[event] ||= []).push(t);
  return o;
};

const point = (c) => ({ text: c.text, cursor: c.cursor });
const clamp = (n, max) => Math.max(0, Math.min(max, n));
function motion(c, name, n) {
  const text = c.text;
  let p = c.cursor;
  for (let i = 0; i < n; i++) {
    if (name === 'right') p = Math.min(text.length, p + (text.codePointAt(p) > 65535 ? 2 : 1));
    if (name === 'left') p = Math.max(0, p - (p > 1 && /[\uDC00-\uDFFF]/.test(text[p - 1]) ? 2 : 1));
    if (name === 'word') p += (text.slice(p).match(/^(?:\w+|[^\w\s]+)?\s*/u)?.[0].length || 1);
    if (name === 'backWord') p = text.slice(0, p).replace(/\s*(?:\w+|[^\w\s]+)\s*$/u, '').length;
    if (name === 'lineStart') p = text.lastIndexOf('\n', p - 1) + 1;
    if (name === 'lineEnd') { const e = text.indexOf('\n', p); p = e < 0 ? text.length : e; }
    if (name === 'start') p = 0;
    if (name === 'end') p = text.length;
    if (name === 'up' || name === 'down') {
      const start = text.lastIndexOf('\n', p - 1) + 1, col = p - start;
      if (name === 'up' && start) { const prev = text.lastIndexOf('\n', start - 2) + 1; p = Math.min(start - 1, prev + col); }
      if (name === 'down') { const end = text.indexOf('\n', p); if (end >= 0) { const next = text.indexOf('\n', end + 1); p = Math.min(next < 0 ? text.length : next, end + 1 + col); } }
    }
    p = clamp(p, text.length);
  }
  return p;
}

/* Text operations are pure datamodel actions. Undo is document history, not
 * SCXML history (which restores control state, used for IME below). */
function execute(context, event) {
  const c = { ...context, notice: '' };
  const command = event.command;
  const n = Math.min(999, event.count ?? (c.count === '' ? 1 : Number(c.count)) * (c.motionCount === '' ? 1 : Number(c.motionCount)));
  if (c.transaction && ['text', 'newline', 'backspace', 'left', 'right', 'up', 'down'].includes(command))
    c.insertEdits = [...c.insertEdits, { command, text: event.text, count: n }];
  const edit = (lo, hi, text) => {
    const next = c.text.slice(0, lo) + text + c.text.slice(hi);
    if (next !== c.text) {
      if (!c.transaction) c.undo = [...c.undo, point(c)].slice(-200);
      c.redo = []; c.revision++; c.text = next;
    }
    c.cursor = lo + text.length;
  };
  const finish = () => {
    if (c.transaction) {
      if (c.transaction.text !== c.text) {
        c.undo = [...c.undo, c.transaction].slice(-200);
        c.lastChange = { command: c.insertCommand, edits: c.insertEdits, count: c.insertCount };
      }
      c.transaction = null; c.insertEdits = [];
    }
  };
  if (command === 'escape') { finish(); c.nextMode = c.keymap === 'vim' ? 'normal' : 'emacs'; c.mark = null; }
  else if (command === 'insert' || command === 'append') {
    c.transaction = point(c); c.insertEdits = []; c.insertCommand = command; c.insertCount = n;
    if (command === 'append') c.cursor = motion(c, 'right', 1);
    c.nextMode = 'insert';
  } else if (command === 'text' || command === 'newline' || command === 'backspace') {
    const value = command === 'newline' ? '\n' : event.text || '';
    if (command === 'backspace') {
      const lo = motion(c, 'left', 1); edit(lo, c.cursor, '');
    } else {
      const lo = c.mark == null ? c.cursor : Math.min(c.cursor, c.mark);
      const hi = c.mark == null ? c.cursor : Math.max(c.cursor, c.mark);
      edit(lo, hi, value); c.mark = null;
    }
  } else if (command === 'undo' || command === 'redo') {
    finish();
    const from = command, to = command === 'undo' ? 'redo' : 'undo';
    if (c[from].length) {
      const saved = c[from].at(-1); c[to] = [...c[to], point(c)]; c[from] = c[from].slice(0, -1);
      c.text = saved.text; c.cursor = saved.cursor; c.revision++; c.mark = null;
    }
  } else if (command === 'visual' || command === 'mark') {
    c.mark = c.mark == null ? c.cursor : null; if (command === 'visual') c.nextMode = 'visual';
  } else if (command.startsWith('record:')) { c.recording = command.split(':')[1]; c.tape = []; }
  else if (command === 'stopRecord') { c.macros = { ...c.macros, [c.recording]: c.tape }; c.recording = ''; }
  else if (command.startsWith('play:')) {
    const tape = c.macros[command.split(':')[1]] || [];
    c.playback = Array.from({ length: n }, () => tape).flat();
    c.notice = tape.length ? 'Replaying actor messages' : 'That register is empty';
  } else if (command === 'repeat') {
    if (c.lastChange) {
      const last = c.lastChange;
      let next = execute({ ...c, count: String(last.count), motionCount: '' }, { command: last.command });
      if (last.edits && next.nextMode === 'insert') {
        next.mode = 'insert';
        for (const edit of last.edits) next = execute(next, edit);
        next = execute(next, { command: 'escape' });
      }
      return { ...next, lastChange: last, count: '', motionCount: '' };
    }
  } else if (command.startsWith('d:') || command.startsWith('c:') || ['delete', 'cut', 'changeSelection'].includes(command)) {
    const change = command.startsWith('c:') || command === 'changeSelection';
    let lo = c.cursor, hi = c.cursor;
    const kind = command.split(':')[1];
    if (kind === 'quote') {
      lo = c.text.lastIndexOf('"', c.cursor - 1);
      if (c.text[c.cursor] === '"') lo = c.cursor;
      hi = c.text.indexOf('"', lo + 1);
      if (lo < 0 || hi < 0 || c.cursor > hi) { c.notice = 'No surrounding double quotes'; c.nextMode = 'normal'; return { ...c, count: '', motionCount: '' }; }
      lo++;
    } else if (kind === 'line') {
      lo = c.text.lastIndexOf('\n', c.cursor - 1) + 1; hi = lo;
      for (let i = 0; i < n; i++) { const e = c.text.indexOf('\n', hi); hi = e < 0 ? c.text.length : e + 1; }
    } else if (kind) hi = motion(c, kind, n);
    else if (c.mark != null) { lo = Math.min(c.mark, c.cursor); hi = Math.max(c.mark, c.cursor) + (c.mode === 'visual' ? 1 : 0); }
    else hi = motion(c, 'right', n);
    if (lo > hi) [lo, hi] = [hi, lo];
    if (change) { c.transaction = point(c); c.insertCommand = command; c.insertCount = n; c.insertEdits = []; }
    edit(lo, hi, ''); c.mark = null;
    c.nextMode = change ? 'insert' : c.keymap === 'vim' ? 'normal' : 'emacs';
    if (!change) c.lastChange = { command, count: n };
  } else if (command !== 'save') c.cursor = motion(c, command, n);
  return { ...c, count: '', motionCount: '' };
}

export function editorMachine({ createMachine, assign, raise, sendTo }, { keymap = 'vim', escape = 'j j', timeout = 650, autosave = 1800, latency = 900, replayDelay = 65 } = {}) {
  const maps = keyBindings(keymap, escape);
  const idle = '#sequenceIdle';
  const reset = assign({ prefix: '', count: '', motionCount: '' });
  const command = (name) => raise({ type: 'DO', command: name });
  const text = (value) => raise(typeof value === 'string' ? { type: 'DO', command: 'text', text: value } : ({ event }) => ({ type: 'DO', command: 'text', text: event.key }));
  const flush = raise(({ context }) => context.mode === 'insert' && context.prefix ? { type: 'DO', command: 'text', text: context.prefix.replaceAll(' ', '') } : { type: 'NOTHING' });
  const count = assign(({ context, event }) => context.mode === 'operator' ? { motionCount: (context.motionCount + event.key).slice(0, 3) } : { count: (context.count + event.key).slice(0, 3) });
  const roots = {};
  const starts = []; // [event, transition] pairs: the first key of every binding, per mode
  for (const [mode, bindings] of Object.entries(maps)) {
    const root = { children: {} };
    for (const [sequence, action] of Object.entries(bindings)) {
      let node = root;
      for (const key of sequence.split(' ')) node = node.children[key] ||= { children: {} };
      node.command = action;
    }
    const build = (node, keys) => {
      const id = `keys_${mode}_${keys.map((k) => Array.from(k).map((c) => c.codePointAt(0).toString(16)).join('_')).join('__')}`;
      const children = Object.entries(node.children);
      if (!children.length) return { id, entry: command(node.command), always: idle };
      const isOperator = mode === 'normal' && ['d', 'c'].includes(keys[0]);
      return {
        id, initial: 'waiting',
        entry: [assign({ prefix: keys.join(' ') }), ...(isOperator ? [raise({ type: 'OPERATOR' })] : [])],
        on: on(
          // after an operator a digit is a count, and 0 one only after another digit
          isOperator ? [['stroke.char.digit.0', { guard: ({ context }) => context.motionCount !== '', target: '.waiting', reenter: true, actions: count }]] : [],
          children.map(([key], i) => [stroke(key), { target: `.key${i}` }]),
          isOperator ? [['stroke.char.digit', { target: '.waiting', reenter: true, actions: count }]] : [],
          [['stroke', { target: idle, actions: [...(mode === 'insert' ? [text(keys.join('')), raise(({ event }) => ({ ...event }))] : [raise({ type: 'ABORT' })]), reset] }]],
        ),
        states: {
          waiting: { after: { timeout: { target: idle, actions: [node.command ? command(node.command) : mode === 'insert' ? text(keys.join('')) : raise({ type: 'ABORT' }), reset] } } },
          ...Object.fromEntries(children.map(([key, child], i) => [`key${i}`, build(child, [...keys, key])])),
        },
      };
    };
    for (const [key, node] of Object.entries(root.children)) {
      const state = build(node, [key]); roots[state.id] = state;
      starts.push([stroke(key), { guard: ({ context }) => context.mode === mode, target: `#${state.id}` }]);
    }
  }
  const modeState = (name) => ({ entry: assign({ mode: name, nextMode: name }) });
  return createMachine({
    id: 'editor', type: 'parallel',
    context: ({ input = {} }) => ({
      text: input.text ?? sampleText, cursor: input.cursor ?? 0, keymap, mode: keymap === 'vim' ? 'normal' : 'emacs', nextMode: 'normal',
      mark: null, count: '', motionCount: '', prefix: '', undo: [], redo: [], transaction: null,
      insertEdits: [], insertCommand: '', insertCount: 1, lastChange: null, recording: '', tape: [], macros: {}, playback: [], replaying: false,
      revision: 0, savedRevision: 0, savedText: input.text ?? sampleText, failNext: false, request: 0, notice: '', version: 1,
    }),
    on: {
      // a key press: Escape (C-g in Emacs) at once, nothing during a replay, else the sequence's stroke
      'key.special.escape': [
        { guard: ({ context }) => context.replaying, actions: [raise({ type: 'STOP_MACRO' }), raise({ type: 'ESCAPE' })] },
        { actions: raise({ type: 'ESCAPE' }) },
      ],
      'key.ctrl.g': [
        { guard: ({ context }) => context.replaying, actions: [] },
        { guard: ({ context }) => context.keymap === 'emacs', actions: raise({ type: 'ESCAPE' }) },
        { actions: raise(({ event }) => ({ ...event, type: `stroke.${event.type.slice('key.'.length)}` })) },
      ],
      key: [
        { guard: ({ context }) => context.replaying, actions: [] },
        { actions: raise(({ event }) => ({ ...event, type: `stroke.${event.type.slice('key.'.length)}` })) },
      ],
      TEXT: { guard: ({ context }) => !context.replaying && ['insert', 'emacs'].includes(context.mode), actions: [raise({ type: 'FLUSH_PREFIX' }), raise(({ event }) => ({ type: 'PASTE', text: event.text }))] },
      PASTE: { actions: raise(({ event }) => ({ type: 'DO', command: 'text', text: event.text })) },
      DO: { actions: [
        assign(({ context, event }) => ({ tape: context.recording && !/^(record:|play:|stopRecord)/.test(event.command) ? [...context.tape, { ...event, count: (Number(context.count) || 1) * (Number(context.motionCount) || 1) }] : context.tape })),
        assign(({ context, event }) => execute(context, event)),
        raise(({ context }) => ({ type: 'MODE', mode: context.nextMode })), raise({ type: 'EDITED' }),
        raise(({ event }) => ({ type: event.command === 'save' ? 'SAVE' : event.command.startsWith('play:') ? 'PLAY' : 'NOTHING' })),
      ] },
      FAIL_NEXT: { actions: assign({ failNext: ({ context }) => !context.failNext }) },
      SELECT: { actions: assign(({ context, event }) => ({ cursor: clamp(event.cursor, context.text.length), mark: event.mark == null ? null : clamp(event.mark, context.text.length) })) },
      'error.platform.upgrade': { actions: [command('escape'), assign({ keymap, version: ({ context }) => context.version + 1 }), raise({ type: 'ABORT' }), raise({ type: 'MODE', mode: keymap === 'vim' ? 'normal' : 'emacs' })] },
    },
    states: {
      mode: {
        initial: 'editing',
        states: {
          editing: {
            initial: keymap === 'vim' ? 'normal' : 'emacs',
            on: {
              MODE: ['normal', 'insert', 'visual', 'operator', 'emacs'].map((mode) => ({ guard: ({ event }) => event.mode === mode, target: `.${mode}` })),
              OPERATOR: { target: '.operator' },
              ABORT: [{ guard: ({ context }) => context.mode === 'operator', target: '.normal' }],
              COMPOSITION_START: { guard: ({ context }) => ['insert', 'emacs'].includes(context.mode), target: 'composing', actions: raise({ type: 'FLUSH_PREFIX' }) },
            },
            states: {
              normal: modeState('normal'), insert: modeState('insert'), visual: modeState('visual'), operator: modeState('operator'),
              emacs: { ...modeState('emacs'), initial: 'unmarked', states: {
                unmarked: { always: { guard: ({ context }) => context.mark != null, target: 'marked' } },
                marked: { always: { guard: ({ context }) => context.mark == null, target: 'unmarked' } },
              } },
              remembered: { type: 'history' },
            },
          },
          composing: { on: { key: {}, TEXT: {}, COMPOSITION_END: { target: 'editing.remembered', actions: raise(({ event }) => ({ type: 'DO', command: 'text', text: event.text })) } } },
        },
      },
      sequence: {
        initial: 'idle',
        on: {
          ABORT: { target: '.idle', actions: reset },
          ESCAPE: { target: '.idle', actions: [flush, command('escape'), reset] },
          FLUSH_PREFIX: { target: '.idle', actions: [flush, reset] },
          BLUR: { actions: [raise({ type: 'FLUSH_PREFIX' }), raise({ type: 'ABORT' })] },
          'stroke.special.escape': { target: '.idle', actions: [command('escape'), reset] },
          'stroke.ctrl.g': { target: '.idle', actions: [command('escape'), reset] },
        },
        states: {
          idle: { id: 'sequenceIdle', entry: assign({ prefix: '' }), on: on(
            [['stroke.char.letter.q', { guard: ({ context }) => !!context.recording && context.mode === 'normal', actions: command('stopRecord') }]],
            starts, // 0 is a motion here (stroke.char.digit.0, a start), before it counts below
            [['stroke.ctrl.u', { guard: ({ context }) => context.mode === 'emacs', target: 'argument', actions: assign({ count: '' }) }]],
            [['stroke.char.digit', { guard: ({ context }) => context.mode === 'normal', target: 'counted', actions: count }]],
            [['stroke.special.escape', { actions: command('escape') }], ['stroke.ctrl.g', { actions: command('escape') }]],
            [['stroke.char', { guard: ({ context }) => ['insert', 'emacs'].includes(context.mode), actions: text() }]],
            [['stroke', { actions: reset }]],
          ) },
          counted: { after: { timeout: { target: 'idle', actions: reset } }, on: on(
            [['stroke.char.digit', { target: 'counted', reenter: true, actions: count }]], // 0 too, after a digit
            starts,
            [['stroke', { target: 'idle', actions: [reset, raise({ type: 'ABORT' })] }]],
          ) },
          argument: { entry: assign({ prefix: 'C-u' }), after: { timeout: { target: 'idle', actions: reset } }, on: on(
            [['stroke.char.digit', { target: 'argument', reenter: true, actions: count }]],
            starts,
            [['stroke', { target: 'idle', actions: reset }]],
          ) },
          ...roots,
        },
      },
      save: {
        initial: 'clean',
        states: {
          clean: { on: { EDITED: { guard: ({ context }) => context.revision !== context.savedRevision, target: 'dirty' }, SAVE: 'saving' } },
          dirty: { after: { autosave: 'saving' }, on: { EDITED: { target: 'dirty', reenter: true }, SAVE: 'saving' } },
          saving: {
            entry: [assign({ request: ({ context }) => context.request + 1 }), raise(({ context }) => ({ type: 'SAVED', request: context.request, revision: context.revision, text: context.text, failed: context.failNext }), { delay: 'latency' }), assign({ failNext: false })],
            on: { SAVED: [
              { guard: ({ context, event }) => event.request === context.request && event.failed, target: 'failed' },
              { guard: ({ context, event }) => event.request === context.request && event.revision === context.revision, target: 'clean', actions: 'saved' },
              { guard: ({ context, event }) => event.request === context.request, target: 'dirty', actions: 'saved' },
            ] },
          },
          failed: { on: { SAVE: 'saving' } },
        },
      },
      focus: { initial: 'blurred', states: { blurred: { on: { FOCUS: 'focused' } }, focused: { on: { BLUR: 'blurred' } } } },
      macro: {
        initial: 'idle',
        on: { STOP_MACRO: { target: '.idle', actions: assign(({ context }) => ({ macros: context.recording ? { ...context.macros, [context.recording]: context.tape } : context.macros, recording: '', playback: [] })) } },
        states: {
          idle: { always: { guard: ({ context }) => !!context.recording, target: 'recording' }, on: { PLAY: { guard: ({ context }) => context.playback.length > 0 && !context.recording, target: 'playing' } } },
          recording: { always: { guard: ({ context }) => !context.recording, target: 'idle' } },
          playing: {
            entry: assign({ replaying: true }), exit: assign({ replaying: false }),
            after: { replayDelay: { target: 'playing', reenter: true, actions: [sendTo(({ self }) => self, ({ context }) => context.playback[0]), assign({ playback: ({ context }) => context.playback.slice(1) })] } },
            always: { guard: ({ context }) => !context.playback.length, target: 'idle' },
          },
        },
      },
    },
  }, { delays: { timeout, autosave, latency, replayDelay }, actions: { saved: assign({ savedRevision: ({ event }) => event.revision, savedText: ({ event }) => event.text }) } });
}

export async function swapKeymap(api, system, actor, options) {
  // Cancel the old trie before migration; old delayed transition events cannot
  // commit a prefix in the new map. The actor/address and document stay intact.
  actor.send({ type: 'ESCAPE' });
  actor.send({ type: 'ABORT' });
  actor.send({ type: 'STOP_MACRO' });
  system.reload(editorMachine(api, options));
  const result = await system.migrate(actor).done;
  if (result.failed || result.pending.length) throw new Error('Editor chart migration did not finish');
  return result;
}
