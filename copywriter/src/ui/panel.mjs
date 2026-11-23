// The panel: renders a Reviewer's ViewState with the DOM and dispatches its
// commands (AGENTS.md, "The facade: Reviewer"). No framework: a skeleton
// built once (header, main, dock) whose parts are rebuilt only when the
// state they show changed, so the composer keeps its text, focus and caret,
// and the list its scroll position.
//
//   import { mountPanel } from './ui/panel.mjs';
//   const panel = mountPanel(shadowRoot, reviewer, { layout: 'sheet' });
//   panel.unmount();
//
// `root` is a ShadowRoot (the embedded widget) or an element such as
// document.body (the extension's side panel). The stylesheet goes into
// `root` as a <style data-cw-panel>; every selector is under `.cw-panel`,
// and `panelCSS` is exported for a host that adopts it itself.
//
// Words are the last tool: what an icon, a position, a typeface or a
// colour can say has no label (the name is in the tooltip and the
// aria-label), and actions appear where and when they apply.

import {
  createElement,
  ArchiveRestore, ArrowLeft, ArrowRight, ArrowUp, Check, ChevronDown, ChevronRight, CircleSlash, Copy, Download,
  Ellipsis, Eye, EyeOff, FileArchive, KeyRound, LoaderCircle, MessageSquare, MessagesSquare, Mic, Plus, SquarePen, UsersRound,
  MousePointerClick, Pencil, RefreshCw, RotateCcw, Settings, Sparkles, SquareDashedMousePointer, Trash2, TriangleAlert, Undo2, X,
} from 'lucide';
import { panelCSS } from './panel-css.mjs';
import { markPaths, markViewBox } from './mark.mjs';

export { panelCSS };

const SVG = 'http://www.w3.org/2000/svg';
const KEY_URL = 'https://aistudio.google.com/apikey';
const OPEN = new Set(['draft', 'transcribing', 'asking', 'suggested', 'previewing', 'failed']);
const STATES = {
  draft: { icon: MessageSquare, label: 'Note' },
  transcribing: { icon: Mic, label: 'Transcribing' },
  asking: { icon: LoaderCircle, label: 'Asking Gemini', spin: true },
  suggested: { icon: Sparkles, label: 'Suggestions ready' },
  previewing: { icon: Eye, label: 'Previewing on the page' },
  kept: { icon: Check, label: 'Kept' },
  dismissed: { icon: CircleSlash, label: 'Dismissed' },
  failed: { icon: TriangleAlert, label: 'Failed' },
};
const THINKING = [['low', 'Low'], ['medium', 'Medium'], ['high', 'High'], ['xhigh', 'Extra high']];
const PROMPTS = ['Tighten the headline', 'Which claims need a number?', 'Find sentences over 21 words'];
const FAILED = Symbol('failed');  // what send() resolves to when the reviewer refused the command
const HOLD_MS = 350;              // a press shorter than this toggles recording; a longer one is push-to-talk

// --- DOM helpers ------------------------------------------------------------

function h(tag, props, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props ?? {})) {
    if (value == null || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key.startsWith('on') && typeof value === 'function') node.addEventListener(key.slice(2), value);
    else if (key === 'value') node.value = value;
    else if (key === 'checked') node.checked = value;
    else node.setAttribute(key, value === true ? '' : value);
  }
  return append(node, children);
}

function append(node, children) {
  for (const child of children.flat(Infinity)) {
    if (child == null || child === false) continue;
    node.append(child instanceof Node ? child : String(child));
  }
  return node;
}

function icon(node, extra = '') {
  return createElement(node, { class: `cw-icon ${extra}`.trim(), 'aria-hidden': 'true', focusable: 'false' });
}

// An icon button: the label is its tooltip and its accessible name.
function iconButton(node, label, props = {}) {
  const { variant = '', spin = false, ...rest } = props;
  return h('button', { type: 'button', class: `cw-btn cw-btn--icon ${variant}`.trim(), 'aria-label': label, title: label, ...rest },
    icon(node, spin ? 'cw-spin' : ''));
}

function button(text, props = {}, node = null) {
  const { variant = '', ...rest } = props;
  return h('button', { type: 'button', class: `cw-btn ${variant}`.trim(), ...rest }, node && icon(node), text);
}

function mark() {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', markViewBox);
  svg.setAttribute('aria-hidden', 'true');
  for (const d of markPaths) {
    const path = document.createElementNS(SVG, 'path');
    path.setAttribute('d', d);
    path.setAttribute('fill', 'currentColor');
    svg.append(path);
  }
  return svg;
}

function dots() {
  return h('span', { class: 'cw-dots', role: 'status', 'aria-label': 'Thinking', 'data-thinking': '' }, h('i'), h('i'), h('i'));
}

// Markdown-light: paragraphs, "- " lists, **bold**, *italic*, `code`.
// Text nodes only: nothing the model writes becomes markup.
function markdown(text) {
  const out = [];
  for (const block of String(text ?? '').trim().split(/\n{2,}/)) {
    const lines = block.split('\n');
    if (lines.every((line) => /^\s*[-*•]\s+/.test(line))) {
      out.push(h('ul', null, lines.map((line) => h('li', null, inline(line.replace(/^\s*[-*•]\s+/, ''))))));
    } else {
      const p = h('p');
      lines.forEach((line, i) => { if (i) p.append(h('br')); append(p, inline(line)); });
      out.push(p);
    }
  }
  return out;
}

function inline(text) {
  return text.split(/(\*\*[^*]+\*\*|`[^`]+`|\*[^*\s][^*]*\*)/).filter(Boolean).map((part) => {
    if (part.length > 4 && part.startsWith('**') && part.endsWith('**')) return h('strong', null, part.slice(2, -2));
    if (part.length > 2 && part.startsWith('`') && part.endsWith('`')) return h('code', null, part.slice(1, -1));
    if (part.length > 2 && part.startsWith('*') && part.endsWith('*')) return h('em', null, part.slice(1, -1));
    return part;
  });
}

function when(at) {
  const t = typeof at === 'number' ? at : Date.parse(at);
  return Number.isFinite(t) ? new Date(t).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) : '';
}

// How long ago, in a few characters: now, 5 min, 3 h, 2 d, then the date.
function ago(at) {
  const t = typeof at === 'number' ? at : Date.parse(at);
  if (!Number.isFinite(t)) return '';
  const minutes = Math.round((Date.now() - t) / 60000);
  if (minutes < 1) return 'now';
  if (minutes < 60) return `${minutes} min`;
  if (minutes < 60 * 24) return `${Math.round(minutes / 60)} h`;
  if (minutes < 60 * 24 * 7) return `${Math.round(minutes / 1440)} d`;
  return new Date(t).toLocaleDateString([], { month: 'short', day: 'numeric' });
}

function seconds(ms) {
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

// --- the panel ----------------------------------------------------------------

export function mountPanel(root, reviewer, { layout = 'sidebar' } = {}) {
  if (layout !== 'sidebar' && layout !== 'sheet') throw new TypeError(`layout is 'sidebar' or 'sheet', not ${layout}`);
  const doc = root.ownerDocument ?? document;

  let style = root.querySelector?.(':scope > style[data-cw-panel]');
  const ownStyle = !style;
  if (ownStyle) {
    style = doc.createElement('style');
    style.setAttribute('data-cw-panel', '');
    style.textContent = panelCSS;
    root.prepend(style);
  }

  const panel = h('div', { class: 'cw-panel', 'data-layout': layout });
  if (root === doc.body) panel.classList.add('cw-in-page');

  // What the panel itself remembers; never the reviewer's state.
  const ui = {
    view: 'list',          // 'list' | 'chat' | 'settings' (an active annotation shows over list and chat)
    showDone: null,        // the kept and dismissed annotations, folded under the open ones (null: open when nothing else is)
    menu: null,            // 'export' | 'more'
    scope: 'page',
    editing: null,         // the annotation whose own text is being written
    editDraft: '',
    showKey: false,
    keyDraft: '',
    voiceDraft: null,      // the voice's instructions being edited
    audienceOpen: null,    // the audience whose editor is open ('new' for a new one)
    audienceDraft: null,   // its fields being edited
    refreshing: false,
  };
  const rec = { active: false, toggled: false, pendingStart: false, downAt: 0, startedAt: 0, target: null, timer: 0 };
  const seen = new Set();  // ids already drawn, so only new cards and messages animate in
  const keys = {};
  let state = reviewer.getState();
  let lastPlace = null;
  let lastChosen = null;   // the card previewed when last drawn: a new one is scrolled into view, Keep with it
  let lastCount = 0;
  let alive = true;
  let scheduled = false;
  let pointer = '';        // the last press's pointerType: a touch opens no keyboard by itself

  const send = (command) => {
    let result;
    try { result = reviewer.dispatch(command); } catch (error) { result = Promise.reject(error); }
    return Promise.resolve(result).catch((error) => {
      if (alive) toast(error?.message || `${command.type} failed`, 'error');
      return FAILED;
    });
  };

  const activeAnnotation = () => (state.activeID && ui.view !== 'settings'
    ? state.annotations?.find((a) => a.id === state.activeID) ?? null
    : null);

  // --- skeleton -----------------------------------------------------------------

  const lead = h('div', { class: 'cw-lead', style: 'display:flex' });
  const heading = h('div', { class: 'cw-heading' });
  const moreAnchor = h('div', { class: 'cw-anchor', 'data-anchor': 'more' },
    iconButton(Ellipsis, 'More', { 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'data-menu': 'more', onclick: () => toggleMenu('more') }));
  const chatButton = iconButton(MessagesSquare, 'Page chat', {
    'data-action': 'chat', 'aria-pressed': 'false',
    onclick: () => go(ui.view === 'chat' ? 'list' : 'chat'),
  });
  const newChatButton = iconButton(SquarePen, 'New conversation', {
    'data-action': 'new-chat', onclick: () => send({ type: 'chat.new' }),
  });
  const settingsButton = iconButton(Settings, 'Settings', {
    'data-action': 'settings', 'aria-pressed': 'false',
    onclick: () => go(ui.view === 'settings' ? 'list' : 'settings'),
  });
  const exportAnchor = h('div', { class: 'cw-anchor', 'data-anchor': 'export' },
    iconButton(Download, 'Export', { 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'data-menu': 'export', onclick: () => toggleMenu('export') }));
  const header = h('header', { class: 'cw-header' }, lead, heading, moreAnchor, newChatButton, chatButton, settingsButton, exportAnchor);

  // a div, not <main>: the host page keeps its one main landmark
  const main = h('div', { class: 'cw-main', tabindex: '-1' });
  const banner = h('div');
  const textarea = h('textarea', { rows: '1', 'aria-label': 'Message', enterkeyhint: 'send' });
  const recLine = h('div', { class: 'cw-rec', hidden: true, 'aria-live': 'polite' });
  const mic = iconButton(Mic, 'Hold to talk', { variant: 'cw-mic', 'aria-pressed': 'false', 'data-action': 'mic' });
  const sendButton = iconButton(ArrowUp, 'Send', { variant: 'cw-send', 'data-action': 'send', disabled: true });
  const pickButton = iconButton(SquareDashedMousePointer, 'Pick on the page', {
    variant: 'cw-pick', 'aria-pressed': 'false', 'data-action': 'pick', onclick: () => togglePick(),
  });
  const composer = h('div', { class: 'cw-composer' }, pickButton, textarea, recLine, mic, sendButton);
  const dock = h('div', { class: 'cw-dock' }, banner, composer);
  const toasts = h('div', { class: 'cw-toasts', 'aria-live': 'polite' });
  panel.append(header, main, dock, toasts);
  root.append(panel);

  function go(view) {
    ui.view = view;
    if (view !== 'settings') { ui.audienceOpen = null; ui.audienceDraft = null; }
    ui.menu = null;
    if (view !== 'settings' && state.activeID) send({ type: 'deselect', id: state.activeID });
    render();
    focusLead();
  }

  // Pick mode: a tap on the page picks its text. Starting it leaves an open
  // annotation and the chat, so a message annotates the pick.
  function togglePick() {
    if (state.picking) { send({ type: 'pick.stop' }); return; }
    if (activeAnnotation()) send({ type: 'deselect', id: state.activeID });
    if (ui.view !== 'list') { ui.view = 'list'; render(); }
    send({ type: 'pick.start' });
  }

  function back() {
    const a = activeAnnotation();
    ui.menu = null;
    ui.editing = null;
    if (a) send({ type: 'deselect', id: a.id });
    else ui.view = 'list';
    render();
    focusLead();
  }

  function focusLead() {
    queueMicrotask(() => {
      if (!panel.contains(focused()) || focused() === main) (lead.querySelector('button') ?? main).focus({ preventScroll: true });
    });
  }

  function focused() {
    return root.activeElement ?? doc.activeElement;
  }

  // --- composer -----------------------------------------------------------------

  // Where a message goes: the active annotation, else a new annotation of
  // the selection (its note asks at once), else the page chat.
  function mode() {
    if (ui.view === 'settings') return { kind: 'none' };
    const a = activeAnnotation();
    if (a) return { kind: 'ask', id: a.id, busy: a.state === 'asking' || a.state === 'transcribing', target: a.id };
    if (ui.view !== 'chat' && state.selection && state.selection.fits !== false) return { kind: 'annotate', busy: false, target: 'page' };
    return { kind: 'chat', busy: !!state.chat?.busy, target: 'page' };
  }

  function canSend() {
    const m = mode();
    return m.kind !== 'none' && !m.busy && !!state.settings?.hasKey && state.status === 'ready' && textarea.value.trim() !== '';
  }

  function autosize() {
    textarea.style.height = 'auto';
    textarea.style.height = `${Math.min(textarea.scrollHeight, 168)}px`;
  }

  function submit() {
    if (!canSend()) return;
    const text = textarea.value.trim();
    const m = mode();
    textarea.value = '';
    autosize();
    if (m.kind === 'ask') send({ type: 'ask', id: m.id, text });
    else if (m.kind === 'annotate') annotate(text);
    else { ui.view = 'chat'; send({ type: 'chat', text }); }
    render();
  }

  // annotate resolves to the new annotation's id: open it, unless the
  // reviewer already did or the user went elsewhere meanwhile; then the
  // composer has the focus (but on touch, where it would raise a keyboard)
  function annotate(note) {
    const before = state.activeID;
    const typing = pointer !== 'touch';
    send(note ? { type: 'annotate', note } : { type: 'annotate' }).then((id) => {
      if (typeof id !== 'string' || !alive || ui.view === 'settings') return;
      if (state.activeID === before && state.activeID !== id) send({ type: 'select', id });
      if (typing) setTimeout(() => { if (alive && state.activeID === id && !textarea.hidden) textarea.focus({ preventScroll: true }); });
    });
  }

  textarea.addEventListener('input', () => { autosize(); sendButton.disabled = !canSend(); });
  textarea.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing && event.keyCode !== 229) {
      event.preventDefault();
      submit();
    }
  });
  sendButton.addEventListener('click', submit);

  // Push-to-talk: press and hold records until release; a short tap
  // toggles (tap to start, tap to stop), for touch and the keyboard.
  function startRecording() {
    const m = mode();
    if (m.kind === 'none' || !state.settings?.hasKey) return;
    Object.assign(rec, { active: true, toggled: false, pendingStart: true, target: m.target, startedAt: performance.now() });
    clearInterval(rec.timer);
    rec.timer = setInterval(paintComposer, 500);
    send({ type: 'record.start', target: m.target });
    paintComposer();
  }
  function stopRecording() {
    if (!rec.active) return;
    Object.assign(rec, { active: false, toggled: false, pendingStart: false });
    clearInterval(rec.timer);
    send({ type: 'record.stop', target: rec.target });
    paintComposer();
  }
  mic.addEventListener('pointerdown', (event) => {
    if (event.button !== 0 || mic.disabled) return;
    event.preventDefault();
    if (rec.active) { stopRecording(); return; }
    rec.downAt = performance.now();
    try { mic.setPointerCapture(event.pointerId); } catch { /* not capturable */ }
    startRecording();
  });
  mic.addEventListener('pointerup', () => {
    if (!rec.active || rec.toggled) return;
    if (performance.now() - rec.downAt < HOLD_MS) { rec.toggled = true; paintComposer(); } else stopRecording();
  });
  mic.addEventListener('pointercancel', () => { if (rec.active && !rec.toggled) stopRecording(); });
  mic.addEventListener('contextmenu', (event) => event.preventDefault());
  mic.addEventListener('keydown', (event) => {
    if ((event.key === 'Enter' || event.key === ' ') && !event.repeat) {
      event.preventDefault();
      if (rec.active) stopRecording(); else { startRecording(); rec.toggled = true; paintComposer(); }
    }
  });

  function paintComposer() {
    const m = mode();
    const hasKey = !!state.settings?.hasKey;
    const recording = rec.active || state.recording?.state === 'recording';
    const transcribing = state.recording?.state === 'transcribing';
    dock.hidden = m.kind === 'none';
    // toasts sit above the composer, or at the bottom without one, never over a label
    toasts.style.bottom = `${(dock.hidden ? 0 : dock.offsetHeight) + 12}px`;

    composer.classList.toggle('is-recording', recording);
    textarea.hidden = recording || transcribing;
    recLine.hidden = !(recording || transcribing);
    if (recording) recLine.replaceChildren(h('span', { class: 'cw-rec-dot' }), seconds(performance.now() - (rec.startedAt || performance.now())));
    else if (transcribing) recLine.replaceChildren(icon(LoaderCircle, 'cw-spin'), h('span', { class: 'cw-sr' }, 'Transcribing'));

    const micLabel = recording ? (rec.toggled ? 'Send recording' : 'Release to send') : 'Hold to talk';
    mic.setAttribute('aria-pressed', String(recording));
    mic.setAttribute('aria-label', micLabel);
    mic.title = recording ? micLabel : 'Hold to talk, or tap to start and stop';
    mic.disabled = !hasKey || transcribing || state.status !== 'ready';

    textarea.placeholder = !hasKey ? 'Add a Gemini key in settings to ask'
      : state.picking && !state.selection ? 'Tap text on the page'
        : { ask: 'Ask for a change', annotate: 'What should change?', chat: 'Ask about this page' }[m.kind] ?? '';
    pickButton.hidden = recording || transcribing;
    pickButton.disabled = state.status !== 'ready';
    pickButton.setAttribute('aria-pressed', String(!!state.picking));
    pickButton.setAttribute('aria-label', state.picking ? 'Stop picking' : 'Pick on the page');
    pickButton.title = state.picking ? 'Stop picking (Esc)' : 'Pick on the page';
    sendButton.disabled = !canSend();
  }

  // --- menus --------------------------------------------------------------------

  function toggleMenu(name) {
    ui.menu = ui.menu === name ? null : name;
    render();
    if (ui.menu) panel.querySelector('.cw-menu button:not([disabled])')?.focus();
  }

  function closeMenu(refocus) {
    const name = ui.menu;
    ui.menu = null;
    render();
    if (refocus) panel.querySelector(`[data-menu="${name}"]`)?.focus();
  }

  function exportMenu() {
    const building = state.exporting === 'building';
    const pageKept = (state.annotations ?? []).filter((a) => a.state === 'kept').length;
    const pageOpen = (state.annotations ?? []).filter((a) => OPEN.has(a.state)).length;
    const run = (to) => {
      ui.menu = null;
      render();
      send({ type: 'export', scope: ui.scope, to }).then((r) => {
        if (r !== FAILED) toast(to === 'clipboard' ? 'Copied for agent' : 'Work packet downloaded');
      });
    };
    // the count is what the export carries: the kept changes and the open items
    const scope = (value, text, kept, open) => h('button', {
      type: 'button', 'aria-pressed': String(ui.scope === value), 'data-scope': value,
      title: `${kept} kept · ${open} open`, 'aria-label': `${text}: ${kept} kept, ${open} open`,
      onclick: () => { ui.scope = value; render(); panel.querySelector(`[data-scope="${value}"]`)?.focus(); },
    }, text, h('span', { class: 'cw-count' }, kept + open));
    return h('div', { class: 'cw-menu', role: 'menu', 'aria-label': 'Export' },
      h('div', { class: 'cw-scope', role: 'group', 'aria-label': 'Scope' },
        scope('page', 'This page', pageKept, pageOpen), scope('all', 'All pages', state.totals?.kept ?? pageKept, state.totals?.open ?? pageOpen)),
      h('button', {
        type: 'button', class: 'cw-menu-item', role: 'menuitem', disabled: building, 'data-action': 'export-download',
        title: 'packet.xml, CHANGES.md and README.md', onclick: () => run('download'),
      }, icon(building ? LoaderCircle : FileArchive, building ? 'cw-spin' : ''), 'Download ZIP'),
      h('button', {
        type: 'button', class: 'cw-menu-item', role: 'menuitem', disabled: building, 'data-action': 'export-clipboard',
        title: 'CHANGES.md, for a coding agent', onclick: () => run('clipboard'),
      }, icon(Copy), 'Copy for agent'));
  }

  function moreMenu(a) {
    const item = (node, text, command, variant = '') => h('button', {
      type: 'button', class: `cw-menu-item ${variant}`.trim(), role: 'menuitem', 'data-action': command.type,
      onclick: () => { ui.menu = null; render(); send(command); },
    }, icon(node), text);
    return h('div', { class: 'cw-menu', role: 'menu', 'aria-label': 'Annotation' },
      a.anchorFound !== false && item(MousePointerClick, 'Show on page', { type: 'select', id: a.id }),
      a.state === 'dismissed'
        ? item(ArchiveRestore, 'Reopen', { type: 'reopen', id: a.id })
        : item(CircleSlash, 'Dismiss', { type: 'dismiss', id: a.id }),
      h('div', { class: 'cw-menu-sep' }),
      item(Trash2, 'Delete', { type: 'delete', id: a.id }, 'cw-btn--danger'));
  }

  function placeMenus() {
    const a = activeAnnotation();
    for (const anchor of [exportAnchor, moreAnchor]) {
      const name = anchor.dataset.anchor;
      anchor.querySelector('[data-menu]').setAttribute('aria-expanded', String(ui.menu === name));
      const open = ui.menu === name && (name === 'export' || a);
      const key = open ? JSON.stringify([ui.scope, state.exporting, state.totals, name === 'more' ? [a.id, a.state, a.anchorFound] : state.annotations?.map((x) => x.state)]) : '';
      if (anchor.dataset.key === key) continue;
      anchor.dataset.key = key;
      anchor.querySelector(':scope > .cw-menu')?.remove();
      if (open) anchor.append(name === 'export' ? exportMenu() : moreMenu(a));
    }
  }

  // --- toasts -------------------------------------------------------------------

  function toast(text, tone = 'ok') {
    const node = h('div', { class: `cw-toast cw-toast--${tone}`, role: tone === 'error' ? 'alert' : 'status' },
      icon(tone === 'error' ? TriangleAlert : Check), h('span', null, text),
      iconButton(X, 'Dismiss', { onclick: () => node.remove() }));
    toasts.append(node);
    setTimeout(() => node.remove(), tone === 'error' ? 6000 : 3000);
  }

  // --- views --------------------------------------------------------------------

  function listView() {
    const view = h('div', { class: 'cw-view', 'data-view': 'list' });
    if (state.status === 'booting') {
      view.append(h('div', { class: 'cw-skeleton', role: 'status', 'aria-label': 'Loading' },
        h('i', { style: 'width:72%' }), h('i', { style: 'width:90%' }), h('i', { style: 'width:56%' })));
      return view;
    }
    if (state.status === 'error') {
      view.append(h('div', { class: 'cw-error', role: 'alert', style: 'margin:12px' }, icon(TriangleAlert),
        h('span', null, state.error ?? 'The reviewer stopped. Reload the panel.')));
    }
    if (!state.settings?.hasKey) {
      view.append(h('button', { type: 'button', class: 'cw-hintrow', 'data-action': 'add-key', onclick: () => go('settings') },
        icon(KeyRound), h('span', null, 'Add a Gemini key for rewrites'), icon(ChevronRight)));
    }
    const all = state.annotations ?? [];
    const open = all.filter((a) => OPEN.has(a.state));
    const done = all.filter((a) => !OPEN.has(a.state));
    if (open.length === 0) {
      view.append(h('div', { class: 'cw-empty', style: done.length ? 'flex:none;padding:28px 24px' : null },
        h('button', {
          type: 'button', class: 'cw-empty-icon', 'data-action': 'pick-empty', 'aria-label': 'Pick on the page', title: 'Pick on the page',
          onclick: () => togglePick(),
        }, icon(SquareDashedMousePointer)), state.picking ? 'Tap text on the page' : 'Pick text on the page'));
    } else {
      view.append(h('ul', { class: 'cw-list', role: 'list', 'aria-label': 'Open' }, open.map((a) => h('li', null, row(a)))));
    }
    if (done.length) {
      const kept = done.filter((a) => a.state === 'kept').length;
      const dismissed = done.length - kept;
      const shown = ui.showDone ?? open.length === 0;
      view.append(h('button', {
        type: 'button', class: 'cw-disclosure', 'aria-expanded': String(shown), 'data-action': 'done',
        'aria-label': `Done: ${kept} kept, ${dismissed} dismissed`, title: `Done: ${kept} kept · ${dismissed} dismissed`,
        onclick: () => { ui.showDone = !shown; render(); },
      }, icon(ChevronRight, 'cw-chevron'), icon(Check), kept, dismissed > 0 && [icon(CircleSlash), dismissed]));
      if (shown) view.append(h('ul', { class: 'cw-list', role: 'list', 'aria-label': 'Done' }, done.map((a) => h('li', null, row(a)))));
    }
    return view;
  }

  function row(a) {
    const s = STATES[a.state] ?? STATES.draft;
    const chosen = a.suggestions?.find((x) => x.id === a.chosenID);
    const replaced = chosen && (a.state === 'previewing' || a.state === 'kept');
    const latest = a.suggestions?.[a.suggestions.length - 1];
    let sub = null;
    if (replaced) sub = h('span', { class: 'cw-row-sub is-change' }, chosen.text);
    else if (a.state === 'failed') sub = h('span', { class: 'cw-row-sub' }, a.error ?? 'Gemini did not answer');
    else if (a.note) sub = h('span', { class: 'cw-row-sub' }, a.note);
    else if (latest) sub = h('span', { class: 'cw-row-sub' }, latest.text);
    const missing = a.anchorFound === false;
    return h('button', {
      type: 'button', class: `cw-row${replaced ? ' is-replaced' : ''}${missing ? ' is-quiet' : ''}`,
      'data-id': a.id, 'data-state': a.state,
      title: `${s.label}${missing ? ' · no longer on the page' : ''}`,
      onclick: () => send({ type: 'select', id: a.id }),
    },
    h('span', { class: 'cw-row-icon' }, icon(missing ? TriangleAlert : s.icon, s.spin ? 'cw-spin' : ''), h('span', { class: 'cw-sr' }, s.label)),
    h('span', { class: 'cw-row-text' }, h('span', { class: 'cw-row-original' }, a.original), sub));
  }

  // One message. A page chat's reply that made annotations links to them;
  // when its text only listed them (proposed), the links say it instead.
  // A spoken message carries a small mic; a reply that thought shows a
  // quiet "Thought for N s" that unfolds its summary; audience calls are
  // chips that open the audience in Settings.
  function message(m) {
    const id = `m:${m.role}:${m.at}:${m.text.length}`;
    const made = (m.annotations ?? []).map((x) => state.annotations?.find((a) => a.id === x)).filter(Boolean);
    const node = h('div', { class: `cw-msg cw-msg--${m.role === 'user' ? 'user' : 'model'}${seen.has(id) ? '' : ' is-new'}`, title: when(m.at) },
      m.thoughts && thought(m, id),
      !(m.proposed && made.length) && h('div', { class: `cw-msg-body${m.spoken ? ' is-spoken' : ''}` },
        m.spoken && h('span', { class: 'cw-spoken', title: 'Spoken' }, icon(Mic), h('span', { class: 'cw-sr' }, 'Spoken: ')),
        markdown(m.text)),
      made.length > 0 && h('div', { class: 'cw-links' }, made.map(link)),
      m.audiences?.length > 0 && h('div', { class: 'cw-audience-chips' }, m.audiences.map(audienceChip)));
    seen.add(id);
    return node;
  }

  function thought(m, id) {
    const open = ui.thoughtsOpen?.has(id);
    const details = h('details', { class: 'cw-thought', open: !!open, 'data-thought': '' },
      h('summary', null, icon(ChevronRight, 'cw-chevron'), m.seconds ? `Thought for ${m.seconds} s` : 'Thoughts'),
      h('div', { class: 'cw-thought-body' }, markdown(m.thoughts)));
    details.addEventListener('toggle', () => {
      ui.thoughtsOpen ??= new Set();
      if (details.open) ui.thoughtsOpen.add(id); else ui.thoughtsOpen.delete(id);
    });
    return details;
  }

  function audienceChip(c) {
    const audiences = state.settings?.audiences ?? [];
    const audience = audiences.find((x) => x.id === c.id) ?? audiences.find((x) => x.name.toLowerCase() === c.name.toLowerCase());
    const active = audience && (state.settings?.audienceIDs ?? []).includes(audience.id);
    return h('button', {
      type: 'button', class: `cw-audience-chip${active ? ' is-active' : ''}`, 'data-action': 'open-audience', 'data-audience': audience?.id ?? '',
      disabled: !audience, title: `${c.action === 'define' ? 'Audience saved' : 'Writing for it'}${active ? ' · active' : ''}`,
      'aria-label': `Audience: ${c.name}`,
      onclick: () => { if (audience) { ui.audienceOpen = audience.id; ui.audienceDraft = null; go('settings'); } },
    }, icon(UsersRound), h('span', null, c.name));
  }

  // A clip being transcribed: the user's bubble, waiting for its words.
  function hearing() {
    return h('div', { class: 'cw-msg cw-msg--user', 'data-transcribing': '' },
      h('div', { class: 'cw-msg-body is-spoken' }, h('span', { class: 'cw-spoken' }, icon(Mic)), dots()));
  }

  // An annotation the page chat made: its text and the proposal. A tap
  // opens it and shows the proposal on the page.
  function link(a) {
    const s = a.suggestions?.find((x) => x.id === a.chosenID) ?? a.suggestions?.[0];
    return h('button', {
      type: 'button', class: 'cw-link', 'data-action': 'open-annotation', 'data-id': a.id,
      title: STATES[a.state]?.label ?? '', 'aria-label': `Open: ${a.original}`,
      onclick: () => openProposal(a),
    }, h('span', { class: 'cw-link-from' }, a.original),
    s && [icon(ArrowRight, 'cw-link-arrow'), h('span', { class: 'cw-link-to' }, s.text)]);
  }

  async function openProposal(a) {
    if (await send({ type: 'select', id: a.id }) === FAILED) return;
    const first = a.suggestions?.[0];
    if (a.state === 'suggested' && first && a.anchorFound !== false) send({ type: 'preview', id: a.id, suggestionID: first.id });
  }

  function thread(messages, busy, transcribing) {
    const list = h('div', { class: 'cw-thread', 'aria-live': 'polite' });
    for (const m of messages ?? []) list.append(message(m));
    if (transcribing) list.append(hearing());
    else if (busy) list.append(h('div', { class: 'cw-msg cw-msg--model' }, dots()));
    return list;
  }

  const hearingFor = (target) => state.recording?.state === 'transcribing' && state.recording.target === target;

  function chatView() {
    const chat = state.chat ?? { messages: [], busy: false };
    const view = h('div', { class: 'cw-view', 'data-view': 'chat' });
    const body = h('div', { class: 'cw-chat' });
    const transcribing = hearingFor('page') || (chat.busy && chat.messages?.at(-1)?.role !== 'user');
    if (!chat.messages?.length && !chat.busy) {
      body.append(h('div', { class: 'cw-empty' },
        h('span', { class: 'cw-empty-icon' }, icon(MessagesSquare)),
        'Ask Gemini about the whole page',
        state.settings?.hasKey && h('div', { class: 'cw-chips' }, PROMPTS.map((text) => h('button', {
          type: 'button', class: 'cw-chip', onclick: () => send({ type: 'chat', text }),
        }, text)))));
      const earlier = (chat.conversations ?? []).filter((c) => c.id !== chat.id);
      if (earlier.length) {
        body.append(h('ul', { class: 'cw-conversations', role: 'list', 'aria-label': 'Earlier conversations' }, earlier.map((c) => h('li', null,
          h('button', {
            type: 'button', class: 'cw-conversation', 'data-action': 'open-chat', 'data-chat': c.id, title: when(c.at),
            onclick: () => send({ type: 'chat.open', id: c.id }),
          }, h('span', { class: 'cw-conversation-title' }, c.title), h('span', { class: 'cw-conversation-at' }, ago(c.at)))))));
      }
    } else {
      body.append(thread(chat.messages, chat.busy, transcribing));
    }
    if (chat.error && !chat.busy) body.append(h('div', { class: 'cw-error', role: 'alert' }, icon(TriangleAlert), h('span', null, chat.error)));
    view.append(body);
    return view;
  }

  function detailView(a) {
    const hasKey = !!state.settings?.hasKey;
    const asking = a.state === 'asking';
    const missing = a.anchorFound === false;
    const chosen = a.suggestions?.find((x) => x.id === a.chosenID);
    const replaced = chosen && (a.state === 'previewing' || a.state === 'kept');
    const view = h('div', { class: 'cw-view', 'data-view': 'detail', 'data-id': a.id });
    const body = h('div', { class: 'cw-detail' });
    view.append(body);

    body.append(h('blockquote', {
      class: `cw-original${replaced ? ' is-replaced' : ''}${missing ? ' is-missing' : ''}`,
      title: missing ? 'No longer on the page' : null,
    }, a.original, missing && h('span', { class: 'cw-missing', role: 'img', 'aria-label': 'No longer on the page' }, icon(TriangleAlert))));

    // the note: why, in the reviewer's words; quiet under the quote
    const note = h('textarea', {
      class: 'cw-note', rows: '1', 'data-focus-key': `note:${a.id}`, 'aria-label': 'Note',
      placeholder: 'Add a note', value: a.note ?? '',
    });
    const commit = () => { if (note.value !== (a.note ?? '')) send({ type: 'note', id: a.id, text: note.value }); };
    note.addEventListener('change', commit);
    note.addEventListener('keydown', (event) => { if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); note.blur(); } });
    body.append(note);

    // Then the conversation as it happened: each message, and after a
    // model's reply the suggestions it brought; the newest at the bottom.
    const messages = a.messages ?? [];
    const suggestions = a.suggestions ?? [];
    const flow = h('div', { class: 'cw-thread', 'aria-live': 'polite' });
    const cardsAfter = (n) => {
      const here = suggestions.filter((s) => (s.after ?? messages.length) === n);
      if (here.length) flow.append(h('div', { class: 'cw-cards', role: 'list', 'aria-label': 'Suggestions' }, here.map((s) => card(a, s, missing))));
    };
    cardsAfter(0);
    messages.forEach((m, i) => { flow.append(message(m)); cardsAfter(i + 1); });
    if (ui.editing === a.id) flow.append(editor(a));
    if (a.state === 'transcribing') flow.append(hearing());
    if (asking) flow.append(h('div', { class: 'cw-msg cw-msg--model' }, dots()));
    if (flow.childNodes.length) body.append(flow);
    if (a.error && a.state !== 'failed') body.append(h('div', { class: 'cw-hint', role: 'status' }, a.error));

    if (a.state === 'failed') {
      body.append(h('div', { class: 'cw-error', role: 'alert' }, icon(TriangleAlert), h('span', null, a.error ?? 'Gemini did not answer.'),
        hasKey && iconButton(RotateCcw, 'Try again', { 'data-action': 'retry', onclick: () => send({ type: 'suggest', id: a.id }) })));
    }

    if (ui.editing !== a.id && !asking && a.state !== 'transcribing') {
      body.append(h('div', { class: 'cw-tools' },
        hasKey && (a.suggestions?.length
          ? iconButton(Sparkles, 'More suggestions', { disabled: asking, 'data-action': 'suggest', onclick: () => send({ type: 'suggest', id: a.id }) })
          : button('Suggest', { disabled: asking, 'data-action': 'suggest', onclick: () => send({ type: 'suggest', id: a.id }) }, Sparkles)),
        iconButton(Pencil, 'Write your own', {
          'data-action': 'write',
          onclick: () => { ui.editing = a.id; ui.editDraft = chosen?.text ?? a.original; render(); focusEditor(); },
        })));
    }
    return view;
  }

  // A suggestion: its text, set as copy. A tap previews it on the page;
  // the previewed one unfolds its reason and Keep. Edit waits in the corner
  // until the card is hovered or focused.
  function card(a, s, missing) {
    const previewing = s.id === a.chosenID && a.state === 'previewing';
    const kept = s.id === a.chosenID && a.state === 'kept';
    const canPreview = !missing && !previewing && !kept && !['asking', 'dismissed', 'kept'].includes(a.state);
    const fresh = !seen.has(`s:${s.id}`);
    seen.add(`s:${s.id}`);
    const why = s.own ? 'Your version' : s.rationale;
    const corner = h('div', { class: 'cw-card-corner' });
    if (kept) {
      corner.append(h('span', { class: 'cw-kept-mark', title: 'Kept' }, icon(Check), h('span', { class: 'cw-sr' }, 'Kept')));
    } else if (!previewing) {
      corner.append(iconButton(Pencil, 'Edit', {
        variant: 'cw-reveal', 'data-action': 'edit-suggestion',
        onclick: () => { ui.editing = a.id; ui.editDraft = s.text; render(); focusEditor(); },
      }));
    }
    const actions = h('div', { class: 'cw-actions' });
    if (previewing) {
      actions.append(
        iconButton(Undo2, 'Undo', { 'data-action': 'undo', onclick: () => send({ type: 'undo', id: a.id }) }),
        button('Keep', { variant: 'cw-btn--primary', 'data-action': 'keep', onclick: () => send({ type: 'keep', id: a.id }) }, Check));
    } else if (kept) {
      actions.append(iconButton(Undo2, 'Undo', { 'data-action': 'undo', onclick: () => send({ type: 'undo', id: a.id }) }));
    }
    const hasActions = actions.childNodes.length > 0;
    return h('article', {
      class: `cw-card${previewing ? ' is-chosen' : ''}${kept ? ' is-kept' : ''}${fresh ? ' is-new' : ''}`,
      role: 'listitem', 'data-suggestion': s.id,
    },
    h('button', {
      type: 'button', class: 'cw-card-pick', 'data-action': 'preview',
      'aria-disabled': String(!canPreview), 'aria-pressed': String(previewing || kept),
      title: canPreview ? 'Preview on the page' : (missing ? 'No longer on the page' : null),
      onclick: () => { if (canPreview) send({ type: 'preview', id: a.id, suggestionID: s.id }); },
    }, h('div', { class: 'cw-card-text' }, s.text)),
    corner,
    (why || hasActions) && h('div', { class: 'cw-fold' }, h('div', null,
      why && h('div', { class: 'cw-why' }, why), hasActions && actions)));
  }

  function editor(a) {
    const area = h('textarea', { 'data-focus-key': `edit:${a.id}`, 'aria-label': 'Your version', value: ui.editDraft, rows: '2' });
    const ok = button('Preview', { variant: 'cw-btn--primary', disabled: !ui.editDraft.trim(), 'data-action': 'edit-preview' }, Eye);
    const fit = () => { area.style.height = 'auto'; area.style.height = `${area.scrollHeight}px`; };
    area.addEventListener('input', () => { ui.editDraft = area.value; ok.disabled = !area.value.trim(); fit(); });
    area.addEventListener('keydown', (event) => { if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); ok.click(); } });
    ok.addEventListener('click', () => {
      const text = ui.editDraft.trim();
      if (!text) return;
      ui.editing = null;
      ui.editDraft = '';
      render();
      send({ type: 'edit', id: a.id, text });
    });
    queueMicrotask(fit);
    return h('div', { class: 'cw-edit' }, area, h('div', { class: 'cw-actions' },
      iconButton(X, 'Cancel', { onclick: () => { ui.editing = null; render(); } }), ok));
  }

  function focusEditor() {
    queueMicrotask(() => {
      const area = panel.querySelector('.cw-edit textarea');
      if (area) { area.focus(); area.setSelectionRange(area.value.length, area.value.length); }
    });
  }

  // An audience: a switch (active or not) and its name, which opens its editor.
  function audienceRow(a, active, activeIDs) {
    const open = ui.audienceOpen === a.id;
    const toggle = h('input', {
      type: 'checkbox', role: 'switch', class: 'cw-switch', checked: active, 'data-focus-key': `audience-on:${a.id}`,
      'aria-label': `Write for ${a.name}`, title: active ? 'Writing for it' : 'Not writing for it',
    });
    toggle.addEventListener('change', () => send({
      type: 'settings.save', audienceIDs: toggle.checked ? [...activeIDs, a.id] : activeIDs.filter((x) => x !== a.id),
    }));
    return h('div', { class: `cw-audience${open ? ' is-open' : ''}`, 'data-audience': a.id },
      h('div', { class: 'cw-audience-head' }, toggle,
        h('button', {
          type: 'button', class: 'cw-audience-name', 'aria-expanded': String(open), 'data-action': 'edit-audience', title: a.description,
          onclick: () => { ui.audienceOpen = open ? null : a.id; ui.audienceDraft = null; render(); },
        }, h('span', null, a.name), a.source === 'model' && h('span', { class: 'cw-ok', title: 'Defined by Gemini' }, icon(Sparkles)),
        icon(ChevronRight, 'cw-chevron'))),
      open && audienceEditor(a));
  }

  const AUDIENCE_FIELDS = [
    ['description', 'Who they are'], ['knows', 'What they know'], ['wants', 'What they want here'],
    ['tasks', 'What they came to do'], ['objections', 'Their objections'],
  ];

  function audienceEditor(a) {
    ui.audienceDraft ??= { id: a?.id, name: a?.name ?? '', ...Object.fromEntries(AUDIENCE_FIELDS.map(([f]) => [f, a?.[f] ?? ''])) };
    const draft = ui.audienceDraft;
    const key = a?.id ?? 'new';
    const name = h('input', { class: 'cw-input', 'data-focus-key': `audience-name:${key}`, value: draft.name, placeholder: 'Name', 'aria-label': 'Name' });
    name.addEventListener('input', () => { draft.name = name.value; });
    const fields = AUDIENCE_FIELDS.map(([field, label]) => {
      const area = h('textarea', { class: 'cw-textarea cw-textarea--small', rows: '2', 'data-focus-key': `audience-${field}:${key}`, value: draft[field], placeholder: label, 'aria-label': label });
      area.addEventListener('input', () => { draft[field] = area.value; });
      return h('label', { class: 'cw-field' }, h('span', { class: 'cw-field-label' }, label), area);
    });
    const save = () => {
      const audience = { ...draft };
      if (!audience.id) delete audience.id;
      send({ type: 'settings.save', audience }).then((r) => {
        if (r === FAILED) return;
        ui.audienceOpen = null;
        ui.audienceDraft = null;
        render();
      });
    };
    return h('div', { class: 'cw-audience-edit' }, name, fields,
      h('div', { class: 'cw-buttons' },
        a && iconButton(Trash2, 'Delete audience', {
          variant: 'cw-btn--danger', 'data-action': 'delete-audience',
          onclick: () => {
            ui.audienceOpen = null;
            ui.audienceDraft = null;
            send({ type: 'settings.deleteAudience', id: a.id }).then((r) => r !== FAILED && toast(`${a.name} deleted`));
          },
        }),
        h('span', { class: 'cw-grow' }),
        iconButton(X, 'Cancel', { onclick: () => { ui.audienceOpen = null; ui.audienceDraft = null; render(); } }),
        button('Save', { variant: 'cw-btn--secondary', 'data-action': 'save-audience', onclick: save })));
  }

  function settingsView() {
    const s = state.settings ?? {};
    const hasKey = !!s.hasKey;
    const view = h('div', { class: 'cw-view', 'data-view': 'settings' });
    const body = h('div', { class: 'cw-settings' });
    view.append(body);

    // the key
    const key = h('input', {
      type: ui.showKey ? 'text' : 'password', class: 'cw-key', id: 'cw-key', 'data-focus-key': 'key',
      autocomplete: 'off', spellcheck: 'false', value: ui.keyDraft,
      placeholder: hasKey ? 'Paste a new key to replace it' : 'Paste your key',
    });
    const remember = h('input', { type: 'checkbox', checked: !!s.rememberKey, 'data-focus-key': 'remember' });
    const save = button('Save', { variant: 'cw-btn--primary', hidden: !ui.keyDraft.trim(), 'data-action': 'save-key' });
    key.addEventListener('input', () => { ui.keyDraft = key.value; save.hidden = !key.value.trim(); });
    key.addEventListener('keydown', (event) => { if (event.key === 'Enter') { event.preventDefault(); save.click(); } });
    remember.addEventListener('change', () => { if (hasKey && !ui.keyDraft.trim()) send({ type: 'settings.save', rememberKey: remember.checked }); });
    save.addEventListener('click', () => {
      const apiKey = ui.keyDraft.trim();
      if (!apiKey) return;
      ui.keyDraft = '';
      ui.showKey = false;
      render();
      send({ type: 'settings.save', apiKey, rememberKey: remember.checked }).then((r) => {
        if (r !== FAILED) { toast('Key saved'); return; }
        if (!ui.keyDraft) { ui.keyDraft = apiKey; render(); }  // refused: the key stays to fix
      });
    });
    body.append(h('div', { class: 'cw-group' },
      h('label', { class: 'cw-label', for: 'cw-key' }, 'Gemini key',
        hasKey && h('span', { class: 'cw-ok', title: 'Saved' }, icon(Check), h('span', { class: 'cw-sr' }, 'saved'))),
      h('div', { class: 'cw-control' }, key,
        iconButton(ui.showKey ? EyeOff : Eye, ui.showKey ? 'Hide key' : 'Show key', {
          'aria-pressed': String(ui.showKey), 'data-focus-key': 'show-key', 'data-action': 'show-key',
          onclick: () => { ui.showKey = !ui.showKey; render(); },
        })),
      h('label', { class: 'cw-check' }, remember, 'Remember on this device'),
      h('p', { class: 'cw-fine' }, 'Kept in this browser, sent only to Google. ',
        h('a', { href: KEY_URL, target: '_blank', rel: 'noopener noreferrer', 'data-action': 'get-key' }, 'Get a key')),
      h('div', { class: 'cw-buttons' }, save, h('span', { class: 'cw-grow' }),
        hasKey && button('Forget', {
          variant: 'cw-btn--danger', 'data-action': 'forget-key', title: 'Forget the key on this device',
          onclick: () => send({ type: 'settings.forgetKey' }).then((r) => r !== FAILED && toast('Key forgotten')),
        }, Trash2))));

    // the model
    const models = s.models ?? [];
    const select = h('select', { id: 'cw-model', 'data-focus-key': 'model' },
      models.map((m) => h('option', { value: m.id, selected: m.id === s.model }, m.label ?? m.id)));
    if (s.model && !models.some((m) => m.id === s.model)) select.prepend(h('option', { value: s.model, selected: true }, s.model));
    select.addEventListener('change', () => send({ type: 'settings.save', model: select.value }));
    body.append(h('div', { class: 'cw-group' },
      h('label', { class: 'cw-label', for: 'cw-model' }, 'Model'),
      h('div', { class: 'cw-split' },
        h('div', { class: 'cw-control' }, select, icon(ChevronDown, 'cw-caret')),
        iconButton(RefreshCw, 'Refresh models', {
          'data-action': 'refresh-models', disabled: !hasKey || ui.refreshing, spin: ui.refreshing,
          onclick: () => {
            ui.refreshing = true;
            render();
            send({ type: 'models.refresh' }).finally(() => { ui.refreshing = false; render(); });
          },
        }))));
    body.append(h('div', { class: 'cw-group' },
      h('span', { class: 'cw-label', id: 'cw-thinking' }, 'Thinking'),
      h('div', { class: 'cw-scope cw-thinking', role: 'group', 'aria-labelledby': 'cw-thinking', title: 'How long Gemini thinks before it answers' },
        THINKING.map(([value, text]) => h('button', {
          type: 'button', 'aria-pressed': String((s.thinking ?? 'high') === value), 'data-thinking': value,
          onclick: () => { if (s.thinking !== value) send({ type: 'settings.save', thinking: value }); },
        }, text)))));

    // the voice: how to write
    const voice = s.voice ?? { instructions: '', custom: false };
    // the draft follows the saved voice whenever that changes
    if (ui.voiceDraft?.base !== voice.instructions) ui.voiceDraft = { base: voice.instructions, text: voice.instructions };
    const voiceArea = h('textarea', {
      class: 'cw-textarea', id: 'cw-voice', 'data-focus-key': 'voice', rows: '8', value: ui.voiceDraft.text, placeholder: 'How Gemini writes for this site',
    });
    const saveVoice = button('Save', { variant: 'cw-btn--secondary', 'data-action': 'save-voice', hidden: ui.voiceDraft.text === voice.instructions });
    voiceArea.addEventListener('input', () => { ui.voiceDraft.text = voiceArea.value; saveVoice.hidden = voiceArea.value === voice.instructions; });
    saveVoice.addEventListener('click', () => {
      const text = voiceArea.value;
      send({ type: 'settings.save', voice: text }).then((r) => r !== FAILED && toast('Voice saved'));
    });
    body.append(h('div', { class: 'cw-group' },
      h('label', { class: 'cw-label', for: 'cw-voice', title: 'How to write' }, 'Voice'),
      voiceArea,
      h('div', { class: 'cw-buttons' },
        voice.custom && button('Default', {
          'data-action': 'reset-voice', title: 'The website\'s voice again',
          onclick: () => send({ type: 'settings.save', voice: '' }),
        }, RotateCcw),
        h('span', { class: 'cw-grow' }), saveVoice)));

    // the audiences: who reads
    const audiences = s.audiences ?? [];
    const activeIDs = s.audienceIDs ?? [];
    const list = h('ul', { class: 'cw-audiences', role: 'list' });
    for (const a of audiences) list.append(h('li', null, audienceRow(a, activeIDs.includes(a.id), activeIDs)));
    if (ui.audienceOpen === 'new') list.append(h('li', null, audienceEditor(null)));
    body.append(h('div', { class: 'cw-group' },
      h('div', { class: 'cw-label', title: 'Who reads' }, 'Audiences', h('span', { class: 'cw-grow' }),
        ui.audienceOpen !== 'new' && iconButton(Plus, 'Add an audience', {
          'data-action': 'add-audience', onclick: () => { ui.audienceOpen = 'new'; ui.audienceDraft = null; render(); },
        })),
      audiences.length || ui.audienceOpen === 'new' ? list : h('p', { class: 'cw-fine' }, 'Ask the page chat who the page is for.')));
    return view;
  }

  function selectionBanner() {
    if (!state.selection || ui.view === 'settings' || state.status !== 'ready') return null;
    if (state.selection.fits === false) {
      // too much for one annotation: say what fits, then fade
      return h('div', { class: 'cw-banner cw-banner--hint', 'data-banner': 'refused', role: 'status' },
        icon(MousePointerClick), h('span', null, 'Select one sentence, heading or paragraph.'));
    }
    return h('div', { class: 'cw-banner', 'data-banner': 'selection', title: 'Selected on the page' },
      h('div', { class: 'cw-banner-quote' }, state.selection.text),
      button('Annotate', {
        variant: 'cw-btn--primary', 'data-action': 'annotate',
        onclick: () => { if (ui.view === 'chat') ui.view = 'list'; annotate(); },
      }, MessageSquare));
  }

  // --- render -------------------------------------------------------------------

  function place() {
    if (ui.view === 'settings') return 'settings';
    const a = activeAnnotation();
    return a ? `detail:${a.id}` : ui.view;
  }

  function count() {
    const a = activeAnnotation();
    if (a) return (a.messages?.length ?? 0) + (a.suggestions?.length ?? 0) + (a.state === 'asking' || a.state === 'transcribing' ? 1 : 0);
    if (ui.view === 'chat') return (state.chat?.messages?.length ?? 0) + (state.chat?.busy ? 1 : 0);
    return 0;
  }

  function mainKey() {
    if (ui.view === 'settings') return JSON.stringify(['settings', state.settings, ui.showKey, ui.refreshing, ui.audienceOpen]);
    const a = activeAnnotation();
    if (a) return JSON.stringify(['detail', a, ui.editing === a.id, state.settings?.hasKey, state.recording, state.settings?.audiences, state.settings?.audienceIDs]);
    if (ui.view === 'chat') return JSON.stringify(['chat', state.chat, state.settings?.hasKey, state.annotations?.map((x) => [x.id, x.state, x.chosenID]), state.recording, state.settings?.audiences, state.settings?.audienceIDs]);
    return JSON.stringify(['list', state.status, state.error, state.annotations, state.settings?.hasKey, ui.showDone, state.picking]);
  }

  function renderMain() {
    const key = mainKey();
    if (keys.main === key) return;
    keys.main = key;

    // keep focus, caret and scroll across the rebuild
    const active = focused();
    const inMain = !!active && active !== main && main.contains(active);
    const focusKey = inMain ? active.getAttribute('data-focus-key') : null;
    const caret = focusKey && typeof active.selectionStart === 'number' ? [active.selectionStart, active.selectionEnd] : null;
    const scroll = main.scrollTop;
    const nearBottom = main.scrollHeight - main.scrollTop - main.clientHeight < 80;

    const a = activeAnnotation();
    if (ui.editing && ui.editing !== a?.id) ui.editing = null;
    const view = ui.view === 'settings' ? settingsView() : a ? detailView(a) : ui.view === 'chat' ? chatView() : listView();
    const where = place();
    if (where !== lastPlace) view.classList.add('is-new');
    main.replaceChildren(view);

    const again = focusKey && main.querySelector(`[data-focus-key="${CSS.escape(focusKey)}"]`);
    if (again) {
      again.focus({ preventScroll: true });
      if (caret) try { again.setSelectionRange(caret[0], caret[1]); } catch { /* not a text field */ }
    } else if (inMain) {
      main.focus({ preventScroll: true });  // what had focus is gone: keep focus in the panel
    }

    const n = count();
    // a conversation opens at its newest message, the list at its top
    if (where !== lastPlace) main.scrollTop = where === 'chat' || where.startsWith('detail:') ? main.scrollHeight : 0;
    else if (n > lastCount && (nearBottom || where !== 'list')) main.scrollTop = main.scrollHeight;
    else main.scrollTop = scroll;
    const chosen = a && a.state === 'previewing' ? `${a.id}:${a.chosenID}` : null;
    if (chosen && chosen !== lastChosen) main.querySelector('.cw-card.is-chosen')?.scrollIntoView({ block: 'nearest' });
    lastChosen = chosen;
    lastPlace = where;
    lastCount = n;
  }

  function renderHeader() {
    const a = activeAnnotation();
    const atHome = ui.view === 'list' && !a;
    const key = JSON.stringify([atHome, ui.view, !!a, state.page, state.chat?.busy, state.chat?.messages?.length > 0]);
    if (keys.header === key) return;
    const wasHome = keys.home;
    keys.header = key;
    keys.home = atHome;
    if (wasHome !== atHome) {
      lead.replaceChildren(atHome
        ? h('span', { class: 'cw-mark', title: 'Copywriter', role: 'img', 'aria-label': 'Copywriter' }, mark())
        : iconButton(ArrowLeft, 'Back', { 'data-action': 'back', onclick: back }));
    }
    heading.textContent = ui.view === 'settings' ? 'Settings' : (state.page?.title || state.page?.url || '');
    heading.title = ui.view === 'settings' ? '' : (state.page?.url ?? '');
    moreAnchor.hidden = !a;
    chatButton.hidden = !!a;
    newChatButton.hidden = ui.view !== 'chat' || !!a || !state.chat?.messages?.length;
    newChatButton.disabled = !!state.chat?.busy;  // the reply on its way belongs to this conversation
    chatButton.setAttribute('aria-pressed', String(ui.view === 'chat'));
    chatButton.classList.toggle('cw-busy', !!state.chat?.busy && ui.view !== 'chat');
    settingsButton.setAttribute('aria-pressed', String(ui.view === 'settings'));
  }

  function renderBanner() {
    const key = JSON.stringify([state.selection?.text ?? null, state.selection?.fits, ui.view, state.status]);
    if (keys.banner === key) return;
    keys.banner = key;
    banner.replaceChildren(...[selectionBanner()].filter(Boolean));
  }

  function render() {
    scheduled = false;
    if (!alive) return;
    // the reviewer ended a recording the panel thinks is on (an error, a
    // stop from elsewhere); until record.start was seen, the press holds
    if (rec.active && !rec.pendingStart && state.recording?.state !== 'recording') {
      Object.assign(rec, { active: false, toggled: false });
      clearInterval(rec.timer);
    }
    renderHeader();
    renderMain();
    renderBanner();
    placeMenus();
    paintComposer();
  }

  const unsubscribe = reviewer.subscribe((next) => {
    if (next?.recording?.state === 'recording') rec.pendingStart = false;
    state = next;
    if (!scheduled) { scheduled = true; queueMicrotask(render); }
  });

  // Escape goes back: a menu, a recording, an edit, then a view.
  panel.addEventListener('keydown', (event) => {
    pointer = 'keyboard';
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      const menu = event.target.closest?.('.cw-menu');
      if (!menu) return;
      const items = [...menu.querySelectorAll('button:not([disabled])')];
      const at = items.indexOf(event.target);
      items[(at + (event.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length]?.focus();
      event.preventDefault();
      return;
    }
    if (event.key !== 'Escape' || event.defaultPrevented) return;
    event.preventDefault();
    if (ui.menu) closeMenu(true);
    else if (rec.active) stopRecording();
    else if (state.picking) send({ type: 'pick.stop' });
    else if (event.target === textarea && textarea.value) textarea.select();
    else if (ui.editing) { ui.editing = null; render(); }
    else if (ui.view !== 'list' || activeAnnotation()) back();
  });

  // a press outside an open menu closes it
  panel.addEventListener('pointerdown', (event) => {
    pointer = event.pointerType;
    if (!ui.menu) return;
    const anchor = panel.querySelector(`[data-anchor="${ui.menu}"]`);
    if (anchor && !anchor.contains(event.target)) { ui.menu = null; render(); }
  });

  render();

  return {
    unmount() {
      if (!alive) return;
      if (rec.active) stopRecording();
      alive = false;
      clearInterval(rec.timer);
      unsubscribe?.();
      panel.remove();
      if (ownStyle) style.remove();
    },
  };
}
