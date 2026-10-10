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

/* Text operations are pure datamodel actions: the variables in, the changed
 * ones out. Undo is document history, not SCXML history (which restores
 * control state, used for IME below). */
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

/**
 * The editor: a parallel chart of five regions (mode, key sequence, save,
 * focus, macro) whose key sequence is the keymap's trie, one nested state
 * per prefix. Every option is compiled into the chart: a new keymap is a new
 * version of the chart `editor` (swapKeymap migrates a running editor).
 */
export function editorChart({ scxml }, { keymap = 'vim', escape = 'j j', timeout = 650, autosave = 1800, latency = 900, replayDelay = 65 } = {}) {
  const c = scxml.chart();
  const maps = keyBindings(keymap, escape);
  const idle = 'sequenceIdle';
  const ms = (n) => `${n}ms`;
  /* an event with data for the internal queue (<raise> takes no data) */
  const emit = (event) => c.send(undefined, { target: '#_internal', eventexpr: c.expr((s) => event(s).type) }, c.content(c.expr(event)));
  /* variables from a function of the scope */
  const set = (update) => c.script(c.expr((s) => void Object.assign(s, update(s))));
  const reset = set(() => ({ prefix: '', count: '', motionCount: '' }));
  const command = (name) => emit(() => ({ type: 'DO', command: name }));
  const text = (value) => emit((s) => ({ type: 'DO', command: 'text', text: value === undefined ? s._event.data.key : value }));
  const flush = c.if(c.expr(({ mode, prefix }) => mode === 'insert' && !!prefix), emit(({ prefix }) => ({ type: 'DO', command: 'text', text: prefix.replaceAll(' ', '') })));
  const count = set(({ mode, count: n, motionCount, _event }) => (mode === 'operator' ? { motionCount: (motionCount + _event.data.key).slice(0, 3) } : { count: (n + _event.data.key).slice(0, 3) }));
  /* a deadline: a delayed event, cancelled when the state exits */
  const deadline = (event, delay) => [c.onentry(c.send(event, { delay: ms(delay), id: event })), c.onexit(c.cancel(event))];
  const inMode = (...modes) => c.expr(({ mode }) => modes.includes(mode));

  const roots = [];
  const starts = []; /* the first key of every binding, per mode */
  for (const [mode, bindings] of Object.entries(maps)) {
    const root = { children: {} };
    for (const [sequence, action] of Object.entries(bindings)) {
      let node = root;
      for (const key of sequence.split(' ')) node = node.children[key] ||= { children: {} };
      node.command = action;
    }
    const build = (node, keys) => {
      const id = `keys_${mode}_${keys.map((k) => Array.from(k).map((ch) => ch.codePointAt(0).toString(16)).join('_')).join('__')}`;
      const children = Object.entries(node.children);
      if (!children.length) return c.state(id, c.onentry(command(node.command)), c.transition(undefined, idle));
      const isOperator = mode === 'normal' && ['d', 'c'].includes(keys[0]);
      const waiting = `${id}_waiting`;
      const timedOut = `timeout.${id}`;
      return c.state(id, { initial: waiting },
        c.onentry(set(() => ({ prefix: keys.join(' ') })), isOperator && c.raise('OPERATOR')),
        /* after an operator a digit is a count, and 0 one only after another digit */
        isOperator && c.transition('stroke.char.digit.0', waiting, { cond: c.expr(({ motionCount }) => motionCount !== '') }, count),
        children.map(([key]) => c.transition(stroke(key), `${id}__${Array.from(key).map((ch) => ch.codePointAt(0).toString(16)).join('_')}`)),
        isOperator && c.transition('stroke.char.digit', waiting, count),
        c.transition('stroke', idle,
          mode === 'insert' ? [text(keys.join('')), emit((s) => ({ ...s._event.data, type: s._event.name }))] : c.raise('ABORT'),
          reset),
        c.state(waiting,
          deadline(timedOut, timeout),
          c.transition(timedOut, idle, node.command ? command(node.command) : mode === 'insert' ? text(keys.join('')) : c.raise('ABORT'), reset)),
        children.map(([key, child]) => build(child, [...keys, key])));
    };
    for (const [key, node] of Object.entries(root.children)) {
      const state = build(node, [key]);
      roots.push(state);
      starts.push(c.transition(stroke(key), state.attrs.id, { cond: inMode(mode) }));
    }
  }
  const modeState = (name, ...rest) => c.state(name, c.onentry(set(() => ({ mode: name, nextMode: name }))), ...rest);

  return c.scxml('editor', { initial: 'editor', datamodel: 'ecmascript' },
    /* input: text, cursor */
    c.datamodel({
      text: JSON.stringify(sampleText), cursor: 0, keymap: JSON.stringify(keymap), mode: JSON.stringify(keymap === 'vim' ? 'normal' : 'emacs'), nextMode: '"normal"',
      mark: null, count: '""', motionCount: '""', prefix: '""', undo: [], redo: [], transaction: null,
      insertEdits: [], insertCommand: '""', insertCount: 1, lastChange: null, recording: '""', tape: [], macros: c.expr(() => ({})), playback: [], replaying: false,
      revision: 0, savedRevision: 0, savedText: c.expr(({ text: t }) => t), failNext: false, request: 0, notice: '""', version: 1,
    }),
    c.parallel('editor',
      /* a key press: Escape (C-g in Emacs) at once, nothing during a replay or a composition, else the sequence's stroke */
      c.transition('key.special.escape', undefined, { cond: 'replaying' }, c.raise('STOP_MACRO'), c.raise('ESCAPE')),
      c.transition('key.special.escape', undefined, c.raise('ESCAPE')),
      c.transition('key.ctrl.g', undefined, { cond: 'replaying' }),
      c.transition('key.ctrl.g', undefined, { cond: "keymap === 'emacs'" }, c.raise('ESCAPE')),
      c.transition('key', undefined, { cond: c.expr(({ replaying, In }) => replaying || In('composing')) }),
      c.transition('key', undefined, emit((s) => ({ ...s._event.data, type: `stroke.${s._event.name.slice('key.'.length)}` }))),
      c.transition('TEXT', undefined, { cond: c.expr(({ replaying, mode, In }) => !replaying && !In('composing') && ['insert', 'emacs'].includes(mode)) },
        c.raise('FLUSH_PREFIX'), emit((s) => ({ type: 'PASTE', text: s._event.data.text }))),
      c.transition('PASTE', undefined, emit((s) => ({ type: 'DO', command: 'text', text: s._event.data.text }))),
      c.transition('DO', undefined,
        set(({ recording, tape, count: n, motionCount, _event }) => ({
          tape: recording && !/^(record:|play:|stopRecord)/.test(_event.data.command) ? [...tape, { ..._event.data, count: (Number(n) || 1) * (Number(motionCount) || 1) }] : tape,
        })),
        set((s) => execute({ ...s }, s._event.data)),
        emit(({ nextMode }) => ({ type: 'MODE', mode: nextMode })),
        c.raise('EDITED'),
        emit(({ _event }) => ({ type: _event.data.command === 'save' ? 'SAVE' : _event.data.command.startsWith('play:') ? 'PLAY' : 'NOTHING' }))),
      c.transition('FAIL_NEXT', undefined, c.assign('failNext', '!failNext')),
      c.transition('SELECT', undefined, set(({ text: t, _event: { data } }) => ({ cursor: clamp(data.cursor, t.length), mark: data.mark == null ? null : clamp(data.mark, t.length) }))),
      c.transition('error.platform.upgrade', undefined,
        command('escape'),
        set(({ version }) => ({ keymap, version: version + 1 })),
        c.raise('ABORT'),
        emit(() => ({ type: 'MODE', mode: keymap === 'vim' ? 'normal' : 'emacs' }))),

      c.state('mode', { initial: 'editing' },
        c.state('editing', { initial: keymap === 'vim' ? 'normal' : 'emacs' },
          ['normal', 'insert', 'visual', 'operator', 'emacs'].map((m) => c.transition('MODE', m, { cond: c.expr(({ _event }) => _event.data.mode === m) })),
          c.transition('OPERATOR', 'operator'),
          c.transition('ABORT', 'normal', { cond: inMode('operator') }),
          c.transition('COMPOSITION_START', 'composing', { cond: inMode('insert', 'emacs') }, c.raise('FLUSH_PREFIX')),
          modeState('normal'), modeState('insert'), modeState('visual'), modeState('operator'),
          modeState('emacs', { initial: 'unmarked' },
            c.state('unmarked', c.transition(undefined, 'marked', { cond: 'mark != null' })),
            c.state('marked', c.transition(undefined, 'unmarked', { cond: 'mark == null' }))),
          c.history('remembered', c.transition(undefined, keymap === 'vim' ? 'normal' : 'emacs'))),
        c.state('composing',
          c.transition('COMPOSITION_END', 'remembered', emit((s) => ({ type: 'DO', command: 'text', text: s._event.data.text }))))),

      /* the region's transitions to its own states are internal: an external one
       * would leave the parallel state, and every region with it (SCXML 3.13) */
      c.state('sequence', { initial: idle },
        c.transition('ABORT', idle, { type: 'internal' }, reset),
        c.transition('ESCAPE', idle, { type: 'internal' }, flush, command('escape'), reset),
        c.transition('FLUSH_PREFIX', idle, { type: 'internal' }, flush, reset),
        c.transition('BLUR', undefined, c.raise('FLUSH_PREFIX'), c.raise('ABORT')),
        c.transition('stroke.special.escape', idle, { type: 'internal' }, command('escape'), reset),
        c.transition('stroke.ctrl.g', idle, { type: 'internal' }, command('escape'), reset),
        c.state(idle,
          c.onentry(set(() => ({ prefix: '' }))),
          c.transition('stroke.char.letter.q', undefined, { cond: c.expr(({ recording, mode }) => !!recording && mode === 'normal') }, command('stopRecord')),
          starts, /* 0 is a motion here (stroke.char.digit.0, a start), before it counts below */
          c.transition('stroke.ctrl.u', 'argument', { cond: inMode('emacs') }, set(() => ({ count: '' }))),
          c.transition('stroke.char.digit', 'counted', { cond: inMode('normal') }, count),
          c.transition('stroke.special.escape', undefined, command('escape')),
          c.transition('stroke.ctrl.g', undefined, command('escape')),
          c.transition('stroke.char', undefined, { cond: inMode('insert', 'emacs') }, text()),
          c.transition('stroke', undefined, reset)),
        c.state('counted',
          deadline('timeout.counted', timeout),
          c.transition('timeout.counted', idle, reset),
          c.transition('stroke.char.digit', 'counted', count), /* 0 too, after a digit */
          starts,
          c.transition('stroke', idle, reset, c.raise('ABORT'))),
        c.state('argument',
          c.onentry(set(() => ({ prefix: 'C-u' }))),
          deadline('timeout.argument', timeout),
          c.transition('timeout.argument', idle, reset),
          c.transition('stroke.char.digit', 'argument', count),
          starts,
          c.transition('stroke', idle, reset)),
        roots),

      c.state('save', { initial: 'clean' },
        c.state('clean',
          c.transition('EDITED', 'dirty', { cond: 'revision !== savedRevision' }),
          c.transition('SAVE', 'saving')),
        c.state('dirty',
          deadline('autosave', autosave),
          c.transition('autosave', 'saving'),
          c.transition('EDITED', 'dirty'),
          c.transition('SAVE', 'saving')),
        c.state('saving',
          /* the reply of a slow server: it captures the text and revision of the request */
          c.onentry(
            c.assign('request', 'request + 1'),
            c.send('SAVED', { delay: ms(latency) }, c.content(c.expr(({ request, revision, text: t, failNext }) => ({ request, revision, text: t, failed: failNext })))),
            c.assign('failNext', 'false')),
          c.transition('SAVED', 'failed', { cond: c.expr(({ request, _event: { data } }) => data.request === request && data.failed) }),
          c.transition('SAVED', 'clean', { cond: c.expr(({ request, revision, _event: { data } }) => data.request === request && data.revision === revision) },
            set(({ _event: { data } }) => ({ savedRevision: data.revision, savedText: data.text }))),
          c.transition('SAVED', 'dirty', { cond: c.expr(({ request, _event: { data } }) => data.request === request) },
            set(({ _event: { data } }) => ({ savedRevision: data.revision, savedText: data.text })))),
        c.state('failed', c.transition('SAVE', 'saving'))),

      c.state('focus', { initial: 'blurred' },
        c.state('blurred', c.transition('FOCUS', 'focused')),
        c.state('focused', c.transition('BLUR', 'blurred'))),

      c.state('macro', { initial: 'idle' },
        c.transition('STOP_MACRO', 'idle', { type: 'internal' }, set(({ recording, macros, tape }) => ({ macros: recording ? { ...macros, [recording]: tape } : macros, recording: '', playback: [] }))),
        c.state('idle',
          c.transition(undefined, 'recording', { cond: '!!recording' }),
          c.transition('PLAY', 'playing', { cond: 'playback.length > 0 && !recording' })),
        c.state('recording', c.transition(undefined, 'idle', { cond: '!recording' })),
        c.state('playing',
          c.onentry(c.assign('replaying', 'true')),
          c.onexit(c.assign('replaying', 'false')),
          deadline('replay', replayDelay),
          /* replay: the recorded commands as messages to itself, paced */
          c.transition('replay', 'playing',
            c.send(undefined, { eventexpr: 'playback[0].type', targetexpr: '"#_scxml_" + _sessionid' }, c.content(c.expr(({ playback }) => playback[0]))),
            c.assign('playback', 'playback.slice(1)')),
          c.transition(undefined, 'idle', { cond: '!playback.length' })))));
}

/** A running editor to another keymap: a new version of its chart, migrated in place. */
export async function swapKeymap(api, system, actor, options) {
  /* Cancel the old trie before migration; old delayed events cannot commit a
   * prefix in the new map. The actor (its address) and the document stay. */
  actor.send({ type: 'ESCAPE' });
  actor.send({ type: 'ABORT' });
  actor.send({ type: 'STOP_MACRO' });
  system.load([editorChart(api, options)]);
  const result = await system.migrate(actor).done;
  if (result.failed || result.pending.length) throw new Error('Editor chart migration did not finish');
  return result;
}
