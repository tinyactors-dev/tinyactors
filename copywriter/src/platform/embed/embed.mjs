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
import { createDOMPage } from '../dom.mjs';

const HOST_ID = 'tinyactors-copywriter';
const KEY = 'tinyactors-copywriter-key';
const SHEET = 'tinyactors-copywriter-sheet';
const WIDE = '(min-width: 900px)';
const SNAPS = { full: 0.85, half: 0.45 };  // the sheet's heights, of the viewport

const CSS = String.raw`
:host { all: initial; }
/* while the sheet is open, room after the page, so its last lines can
   scroll up above the sheet at half height */
@media not ${WIDE} { :host([data-open]) { display: block; height: calc(${SNAPS.half} * 100dvh); } }
* { box-sizing: border-box; }
.launcher {
  position: fixed; z-index: 2147483000;
  right: calc(16px + env(safe-area-inset-right, 0px));
  bottom: calc(16px + env(safe-area-inset-bottom, 0px));
  width: 44px; height: 44px; padding: 0; border: 0; border-radius: 6px;
  display: grid; place-items: center; cursor: pointer;
  background: #1F5E4A; color: #FFF8F0;
  box-shadow: 0 0 0 1px rgba(18,48,42,.4), 0 8px 24px rgba(40,30,20,.24);
  -webkit-tap-highlight-color: transparent;
  transition: transform .24s cubic-bezier(.2,0,0,1), opacity .24s;
}
/* out of the way while the reader scrolls down; back on the way up, or when they stop */
:host([data-away]) .launcher { transform: translateY(calc(100% + 24px)); opacity: 0; pointer-events: none; }
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
  height: calc(var(--snap, .85) * 100dvh);
  display: flex; flex-direction: column;
  background: #FAF6EE; border-radius: 12px 12px 0 0; overflow: hidden;
  box-shadow: 0 0 0 1px rgba(42,38,34,.12), 0 -12px 40px rgba(40,30,20,.22);
  padding-bottom: env(safe-area-inset-bottom, 0px);
  transform: translateY(calc(100% + 24px)); visibility: hidden;
  transition: transform .28s cubic-bezier(.16,1,.3,1), height .28s cubic-bezier(.16,1,.3,1), visibility 0s .28s;
}
:host([data-open]) .sheet { transform: translateY(var(--drag, 0px)); visibility: visible; transition: transform .28s cubic-bezier(.16,1,.3,1), height .28s cubic-bezier(.16,1,.3,1); }
:host([data-dragging]) .sheet { transition: none; }
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
      if (snap === 'full') { snap = 'half'; show(true); }
      return { bottom: innerHeight * (1 - SNAPS.half) };
    },
  });

  const reviewer = createReviewer({
    page, credentials, download, clipboard,
    wasmURL: new URL('tinyactors.wasm', import.meta.url).href,
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
    if (selected) host.removeAttribute('data-away');
  });

  // The launcher steps aside while the page scrolls down, and comes back
  // when it scrolls up or stops.
  let lastY = scrollY;
  let still = 0;
  addEventListener('scroll', () => {
    const y = scrollY;
    if (y > lastY + 4 && !selected && !open) host.setAttribute('data-away', '');
    else if (y < lastY - 4) host.removeAttribute('data-away');
    lastY = y;
    clearTimeout(still);
    still = setTimeout(() => host.removeAttribute('data-away'), 700);
  }, { passive: true });

  // ---- the sheet -----------------------------------------------------------------

  let panel = null;
  const saved = (() => { try { return JSON.parse(storage('sessionStorage')?.getItem(SHEET) || 'null'); } catch { return null; } })();
  let snap = saved?.snap === 'half' ? 'half' : 'full';

  function remember() {
    try { storage('sessionStorage')?.setItem(SHEET, JSON.stringify({ open, snap })); } catch { /* full */ }
  }

  function show(value) {
    open = value;
    if (open) {
      panel ??= mountPanel(body, reviewer, { layout: 'sheet' });
      host.setAttribute('data-open', '');
      sheet.style.setProperty('--snap', String(SNAPS[snap]));
    } else {
      host.removeAttribute('data-open');
      if (shadow.activeElement && sheet.contains(shadow.activeElement)) launcher.focus();
    }
    sheet.style.removeProperty('--drag');
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

  // Dragging the grip: down closes or lowers the sheet, up raises it; a tap
  // switches between the two heights.
  let drag = null;
  grip.addEventListener('pointerdown', (event) => {
    if (wide.matches || event.target.closest?.('.close')) return;
    drag = { y: event.clientY, t: performance.now(), dy: 0 };
    grip.setPointerCapture(event.pointerId);
    host.setAttribute('data-dragging', '');
  });
  grip.addEventListener('pointermove', (event) => {
    if (!drag) return;
    drag.dy = event.clientY - drag.y;
    const height = SNAPS[snap] * innerHeight;
    const dy = drag.dy < 0 ? Math.max(drag.dy, SNAPS.full * innerHeight - height) : drag.dy;
    sheet.style.setProperty('--drag', `${dy}px`);
  });
  const release = () => {
    if (!drag) return;
    const { dy, t } = drag;
    drag = null;
    host.removeAttribute('data-dragging');
    const velocity = dy / Math.max(1, performance.now() - t);   // px per ms, down positive
    const height = SNAPS[snap] * innerHeight - dy;
    if (Math.abs(dy) < 6) snap = snap === 'full' ? 'half' : 'full';
    else if (height < SNAPS.half * innerHeight * 0.6 || velocity > 1.2 && snap === 'half') { snap = 'full'; show(false); return; }
    else if (velocity > 0.6 || height < (SNAPS.full + SNAPS.half) / 2 * innerHeight) snap = 'half';
    else snap = 'full';
    show(true);
  };
  grip.addEventListener('pointerup', release);
  grip.addEventListener('pointercancel', release);

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
