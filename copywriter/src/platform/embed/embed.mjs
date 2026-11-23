// The embedded widget: the copywriter on www.tinyactors.dev itself, for a
// phone. A launcher in a Shadow DOM host opens the panel as a bottom sheet
// (a drawer on the right from 900px wide); the reviewer runs in the page,
// with the page adapter on the live DOM (../dom.mjs).
//
// The site loads it only on a device that turned it on at /copywriter/
// (localStorage `tinyactors-copywriter` = `on`):
//
//   <script type="module">try{if(localStorage.getItem('tinyactors-copywriter')==='on')import('/copywriter/embed.js')}catch(e){}</script>
//
// The reviewer boots at once, so the changes kept on this page show again
// before the sheet is opened. Whether the sheet is open, and how high,
// lasts for the tab (sessionStorage), so it stays open across pages.

import { createElement, MessageSquarePlus, PenLine, X } from 'lucide';
import { createReviewer } from '../../core/reviewer.mjs';
import { mountPanel } from '../../ui/panel.mjs';
import { createDOMPage, readHTML } from '../dom.mjs';

const HOST_ID = 'tinyactors-copywriter';
const KEY = 'tinyactors-copywriter-key';
const SHEET = 'tinyactors-copywriter-sheet';
const WIDE = '(min-width: 900px)';
// The sheet's height, as a share of the viewport: it opens low, so the page
// stays in view; the reader drags it anywhere between MIN and MAX, and a tap
// on the grip switches between LOW and HIGH.
const SIZE = { MIN: 0.22, LOW: 0.4, HIGH: 0.75, MAX: 0.92 };

const CSS = String.raw`
:host { all: initial; }
/* while the sheet is open, room after the page, so its last lines can
   scroll up above the sheet at half height */
@media not ${WIDE} { :host([data-open]) { display: block; height: var(--room, 40dvh); } }
* { box-sizing: border-box; }
.launcher {
  position: fixed; z-index: 2147483000;
  /* bottom left: the site's own menu button is bottom right on a phone */
  left: calc(16px + env(safe-area-inset-left, 0px));
  bottom: calc(16px + env(safe-area-inset-bottom, 0px));
  width: 44px; height: 44px; padding: 0; border: 0; border-radius: 6px;
  display: grid; place-items: center; cursor: pointer;
  background: #1F5E4A; color: #FFF8F0;
  box-shadow: 0 0 0 1px rgba(18,48,42,.4), 0 8px 24px rgba(40,30,20,.24);
  -webkit-tap-highlight-color: transparent;
  transition: transform .24s cubic-bezier(.2,0,0,1), opacity .24s;
}
.launcher .on-selection { display: none; }
:host([data-selection]) .launcher .on-selection { display: block; }
:host([data-selection]) .launcher .idle { display: none; }
.launcher:hover { background: #194D3D; }
.launcher:active { transform: scale(.96); }
.launcher:focus-visible { outline: 2px solid #1F5E4A; outline-offset: 3px; }
.launcher svg { width: 20px; height: 20px; stroke-width: 1.75; }
.launcher .count {
  position: absolute; top: -6px; right: -6px; min-width: 18px; height: 18px; padding: 0 4px;
  border-radius: 9px; background: #FAF6EE; color: #2A2622; box-shadow: 0 0 0 1px #C9BAA0;
  font: 600 10.5px/18px 'IBM Plex Mono', ui-monospace, monospace; text-align: center;
}
.launcher .count:empty { display: none; }
:host([data-open]) .launcher { opacity: 0; pointer-events: none; transform: scale(.9); }

.sheet {
  position: fixed; z-index: 2147483001; left: 0; right: 0; bottom: 0;
  height: var(--height, 40dvh);
  display: flex; flex-direction: column;
  background: #FAF6EE; border-radius: 12px 12px 0 0; overflow: hidden;
  box-shadow: 0 0 0 1px rgba(42,38,34,.12), 0 -12px 40px rgba(40,30,20,.22);
  padding-bottom: env(safe-area-inset-bottom, 0px);
  transform: translateY(calc(100% + 24px)); visibility: hidden;
  transition: transform .28s cubic-bezier(.16,1,.3,1), visibility 0s .28s;
  will-change: transform;
}
/* one thing moves at a time: the sheet slides in and out (transform); a
   resize changes its height, animated only for a tap's switch */
:host([data-open]) .sheet { transform: translateY(var(--pull, 0px)); visibility: visible; transition: transform .28s cubic-bezier(.16,1,.3,1); }
:host([data-settling]) .sheet { transition: height .22s cubic-bezier(.16,1,.3,1), transform .28s cubic-bezier(.16,1,.3,1); }
:host([data-dragging]) .sheet { transition: none; }
:host([data-dragging]) .body { pointer-events: none; }
/* the panel's header is a handle too: a bigger target than the grip */
@media not ${WIDE} { .cw-header { touch-action: none; } }
.grip {
  flex: none; position: relative; height: 28px; touch-action: none; cursor: grab;
  display: grid; place-items: center; background: inherit;
}
.grip::before { content: ""; width: 36px; height: 4px; border-radius: 2px; background: #C9BAA0; }
.close {
  position: absolute; right: 6px; top: 50%; transform: translateY(-50%);
  width: 32px; height: 28px; padding: 0; border: 0; background: none; color: #5C544B;
  display: grid; place-items: center; cursor: pointer; border-radius: 2px;
}
.close:focus-visible { outline: 2px solid #1F5E4A; }
.close svg { width: 18px; height: 18px; stroke-width: 1.75; }
.body { flex: 1; min-height: 0; }
/* a scroll that reaches the end of the panel's content stops there, not in the page behind */
.sheet, .sheet * { overscroll-behavior: contain; }

@media (prefers-color-scheme: dark) {
  .sheet { background: #201811; box-shadow: 0 0 0 1px #45362A, 0 -12px 40px rgba(0,0,0,.5); }
  .grip::before { background: #5B4838; }
  .close { color: #CBBBA2; }
  .launcher { background: #5FAE8F; color: #120D09; }
  .launcher:hover { background: #86C4AB; }
  .launcher .count { background: #201811; color: #F1E6D2; box-shadow: 0 0 0 1px #5B4838; }
}

@media ${WIDE} {
  .sheet {
    left: auto; top: 0; width: 400px; height: 100dvh; border-radius: 0;
    box-shadow: 0 0 0 1px rgba(42,38,34,.14), -12px 0 40px rgba(40,30,20,.16);
    transform: translateX(calc(100% + 24px));
    transition: transform .28s cubic-bezier(.16,1,.3,1), visibility 0s .28s;
  }
  :host([data-open]) .sheet { transform: none; transition: transform .28s cubic-bezier(.16,1,.3,1); }
  .grip { cursor: default; }
  .grip::before { display: none; }
}
`;

function storage(kind) {
  try { return globalThis[kind]; } catch { return null; }  // refused by the browser's settings
}

/** Credentials in sessionStorage, or localStorage when remembered. */
const credentials = {
  get: () => storage('sessionStorage')?.getItem(KEY) || storage('localStorage')?.getItem(KEY) || null,
  set(key, { remember = false } = {}) {
    (remember ? storage('localStorage') : storage('sessionStorage'))?.setItem(KEY, key);
    (remember ? storage('sessionStorage') : storage('localStorage'))?.removeItem(KEY);
  },
  forget() {
    storage('sessionStorage')?.removeItem(KEY);
    storage('localStorage')?.removeItem(KEY);
  },
  remembered: () => Boolean(storage('localStorage')?.getItem(KEY)),
};

function download(filename, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/zip' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.style.display = 'none';
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

const clipboard = (text) => navigator.clipboard.writeText(text);

const icon = (node) => createElement(node, { 'aria-hidden': 'true', focusable: 'false' });

function start() {
  if (document.getElementById(HOST_ID)) return null;
  const host = document.createElement('div');
  host.id = HOST_ID;
  document.documentElement.append(host);
  const shadow = host.attachShadow({ mode: 'open' });
  const style = document.createElement('style');
  style.textContent = CSS;

  const count = document.createElement('span');
  count.className = 'count';
  const launcher = document.createElement('button');
  launcher.type = 'button';
  launcher.className = 'launcher';
  launcher.setAttribute('aria-label', 'Open copywriter');
  launcher.title = 'Copywriter';
  const idle = icon(PenLine);
  idle.classList.add('idle');
  const onSelection = icon(MessageSquarePlus);
  onSelection.classList.add('on-selection');
  launcher.append(idle, onSelection, count);

  const close = document.createElement('button');
  close.type = 'button';
  close.className = 'close';
  close.setAttribute('aria-label', 'Close copywriter');
  close.title = 'Close';
  close.append(icon(X));
  const grip = document.createElement('div');
  grip.className = 'grip';
  grip.append(close);
  const body = document.createElement('div');
  body.className = 'body';
  const sheet = document.createElement('div');
  sheet.className = 'sheet';
  sheet.setAttribute('role', 'dialog');
  sheet.setAttribute('aria-label', 'Copywriter');
  sheet.append(grip, body);
  shadow.append(style, launcher, sheet);

  // ---- the page adapter ------------------------------------------------------

  const wide = matchMedia(WIDE);
  let open = false;
  let pressedInside = false;  // the last press was on the widget: a selection that went empty is kept
  document.addEventListener('pointerdown', (event) => { pressedInside = event.composedPath().includes(host); }, true);
  const inside = (node) => node === host || host.contains(node);
  const page = createDOMPage({
    exclude: inside,
    keepSelection: () => pressedInside || document.activeElement === host,
    // What the reader sees of the page: above the sheet, lowered to its
    // half height so the text shows; left of the drawer.
    band() {
      if (!open) return null;
      if (wide.matches) return { right: innerWidth - sheet.getBoundingClientRect().width };
      if (size > SIZE.LOW + 0.05) resize(SIZE.LOW, { animate: true });
      return { bottom: innerHeight * (1 - size) };
    },
  });

  // another page of the site: the widget goes there (it boots again on it)
  page.open = (url) => { location.assign(url); };
  const reviewer = createReviewer({
    page, credentials, download, clipboard,
    wasmURL: new URL('tinyactors.wasm', import.meta.url).href,
    // a page of the site as served, read as context() reads this one
    fetchPage: async (url) => {
      const response = await fetch(url, { cache: 'no-cache', signal: AbortSignal.timeout?.(10000) });
      if (!response.ok) throw new Error(`${url}: ${response.status}`);
      return readHTML(await response.text(), url);
    },
  });
  // With a selection the launcher annotates it: it stays in view, and a
  // tap opens the sheet on the new annotation.
  let selected = false;
  reviewer.subscribe((state) => {
    const n = state.annotations.filter((a) => a.state !== 'kept' && a.state !== 'dismissed').length;
    count.textContent = n ? String(n) : '';
    selected = Boolean(state.selection && state.selection.fits !== false);
    host.toggleAttribute('data-selection', selected);
    launcher.setAttribute('aria-label', selected ? 'Annotate the selection' : 'Open copywriter');
    launcher.title = selected ? 'Annotate' : 'Copywriter';
  });

  // Pick mode lowers the sheet to its least height, so the page shows; it
  // rises back to where it was when pick mode ends (an annotation of the
  // pick, the pick button, Escape).
  // While picking, its least height is what shows the header, the pick's
  // banner and the composer whole, measured after each render.
  let wasPicking = false;
  let beforePick = null;
  function fitPick() {
    if (!wasPicking || wide.matches || !open) return;
    const part = (selector) => body.querySelector(selector)?.offsetHeight ?? 0;
    const need = grip.offsetHeight + part('.cw-header') + part('.cw-dock') + 8;
    resize(Math.max(SIZE.MIN, need / innerHeight), { animate: true });
  }
  reviewer.subscribe((state) => {
    if (state.picking === wasPicking) { if (wasPicking) requestAnimationFrame(fitPick); return; }
    wasPicking = state.picking;
    host.toggleAttribute('data-picking', wasPicking);
    if (wide.matches || !open) return;
    if (wasPicking) { beforePick = size; requestAnimationFrame(fitPick); }
    else if (beforePick !== null) { resize(beforePick, { animate: true }); beforePick = null; }
  });

  // The launcher stays where it is: hiding it while the page scrolled left
  // a reader far down a page with no way to start (the maintainer, 2026-11-23).

  // ---- the sheet -----------------------------------------------------------------

  let panel = null;
  const saved = (() => { try { return JSON.parse(storage('sessionStorage')?.getItem(SHEET) || 'null'); } catch { return null; } })();
  const clamp = (value) => Math.min(SIZE.MAX, Math.max(SIZE.MIN, value));
  let size = clamp(Number(saved?.size) || SIZE.LOW);

  function remember() {
    try { storage('sessionStorage')?.setItem(SHEET, JSON.stringify({ open, size })); } catch { /* full */ }
  }

  /** Sets the sheet's height (a share of the viewport) and the room after the page. */
  function resize(value, { animate = false } = {}) {
    size = clamp(value);
    if (animate) {
      host.setAttribute('data-settling', '');
      clearTimeout(resize.settled);
      resize.settled = setTimeout(() => host.removeAttribute('data-settling'), 300);
    }
    sheet.style.setProperty('--height', `${(size * 100).toFixed(2)}dvh`);
    host.style.setProperty('--room', `${(size * 100).toFixed(2)}dvh`);
    remember();
  }

  // The checklist and a conversation with a reader want room: the sheet
  // rises to its high size when lower, and goes back to where it was when
  // the panel leaves them, unless the reader moved it meanwhile.
  let beforeTall = null;
  function raise(on) {
    if (wide.matches) return;
    if (on) {
      if (size < SIZE.HIGH - 0.01) { beforeTall = size; resize(SIZE.HIGH, { animate: true }); }
    } else if (beforeTall !== null) {
      const back = beforeTall;
      beforeTall = null;
      if (Math.abs(size - SIZE.HIGH) < 0.01) resize(back, { animate: true });
    }
  }

  function show(value) {
    open = value;
    if (open) {
      panel ??= mountPanel(body, reviewer, { layout: 'sheet', onTall: raise });
      resize(size);
      host.setAttribute('data-open', '');
    } else {
      host.removeAttribute('data-open');
      if (shadow.activeElement && sheet.contains(shadow.activeElement)) launcher.focus();
    }
    sheet.style.removeProperty('--pull');
    remember();
  }

  launcher.addEventListener('click', () => {
    const annotating = selected;
    show(true);
    if (annotating) reviewer.dispatch({ type: 'annotate' }).catch(() => { /* the panel's banner says why */ });
  });
  close.addEventListener('click', () => show(false));
  shadow.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && open && event.target === close) show(false);
  });

  // Resizing: the grip, and the panel's header outside its buttons, are a
  // handle. The sheet's top follows the finger and stays where it is let go;
  // pulled below its smallest height it slides down, and let go there, or
  // flicked down from low, it closes. A tap on the grip switches between
  // LOW and HIGH. Moves are applied once per frame.
  const INTERACTIVE = 'button, a, input, select, textarea, [role=button], [role=menu], [contenteditable]';
  const handle = (target) => target.closest?.('.grip') && !target.closest('.close')
    || target.closest?.('.cw-header') && !target.closest(INTERACTIVE);
  let drag = null;
  let frame = 0;
  sheet.addEventListener('pointerdown', (event) => {
    if (wide.matches || !open || !handle(event.target) || event.button > 0) return;
    drag = { id: event.pointerId, y: event.clientY, start: size * innerHeight, moved: false, samples: [[performance.now(), event.clientY]], onGrip: Boolean(event.target.closest('.grip')) };
    sheet.setPointerCapture(event.pointerId);
  });
  sheet.addEventListener('pointermove', (event) => {
    if (!drag || event.pointerId !== drag.id) return;
    const dy = event.clientY - drag.y;
    if (!drag.moved && Math.abs(dy) < 6) return;
    if (!drag.moved) { drag.moved = true; host.setAttribute('data-dragging', ''); }
    drag.samples.push([performance.now(), event.clientY]);
    if (drag.samples.length > 6) drag.samples.shift();
    drag.dy = dy;
    cancelAnimationFrame(frame);
    frame = requestAnimationFrame(() => {
      if (!drag) return;
      const wanted = drag.start - drag.dy;
      const least = SIZE.MIN * innerHeight;
      sheet.style.setProperty('--height', `${Math.min(SIZE.MAX * innerHeight, Math.max(least, wanted))}px`);
      sheet.style.setProperty('--pull', `${Math.max(0, least - wanted)}px`);
    });
  });
  const release = (event) => {
    if (!drag || event.pointerId !== drag.id) return;
    const { moved, samples, onGrip, start } = drag;
    const dy = drag.dy || 0;
    drag = null;
    cancelAnimationFrame(frame);
    host.removeAttribute('data-dragging');
    if (!moved) {
      if (onGrip && event.type === 'pointerup') resize(size > (SIZE.LOW + SIZE.HIGH) / 2 ? SIZE.LOW : SIZE.HIGH, { animate: true });
      return;
    }
    const [t0, y0] = samples[0];
    const [t1, y1] = samples[samples.length - 1];
    const velocity = (y1 - y0) / Math.max(1, t1 - t0);   // px per ms, down positive, over the last moves
    const wanted = (start - dy) / innerHeight;
    sheet.style.removeProperty('--pull');
    beforeTall = null;  // where the reader put it stays
    if (wanted < SIZE.MIN - 0.06 || (velocity > 1.1 && wanted < SIZE.LOW)) { show(false); return; }
    resize(wanted);
  };
  sheet.addEventListener('pointerup', release);
  sheet.addEventListener('pointercancel', release);

  // Open on the last page: it stays where it was, without sliding in again.
  if (saved?.open) {
    host.setAttribute('data-dragging', '');
    show(true);
    requestAnimationFrame(() => requestAnimationFrame(() => host.removeAttribute('data-dragging')));
  }
  return { host, reviewer, show };
}

const widget = start();
if (widget) globalThis.tinyactorsCopywriter = widget;
