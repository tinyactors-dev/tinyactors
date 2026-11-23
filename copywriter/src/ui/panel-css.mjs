// The panel's stylesheet, as a string, so the panel needs no bundler
// loader: mountPanel puts it in a <style> inside its root (a ShadowRoot or
// a document), and a host may adopt `panelCSS` itself.
//
// Every selector is under `.cw-panel`. Colours, type, radii and motion are
// the tinyactors design system's (.agents/skills/tinyactors-design,
// tokens/*.css), redeclared as custom properties on `.cw-panel` so a page's
// own variables cannot leak in; the dark theme follows prefers-color-scheme
// unless `data-theme` on the panel says otherwise.
//
// Type: the page's copy (an original, a suggestion) is set in Newsreader,
// the interface in IBM Plex Sans, labels in IBM Plex Mono, with the
// system's fonts as fallbacks. @font-face does not work inside a shadow
// root, so the page (or the extension's panel page) loads the fonts.

const light = `
  --bg: var(--vellum-50); --bg-sunken: var(--vellum-100);
  --surface: #FFFCF6; --surface-2: var(--vellum-150); --surface-3: var(--vellum-200);
  --fg-1: var(--graphite-900); --fg-2: var(--graphite-600); --fg-3: var(--graphite-500); --fg-4: var(--graphite-400);
  --border-1: var(--vellum-300); --border-2: var(--vellum-400); --border-strong: var(--graphite-500);
  --accent: var(--pine-500); --accent-hover: var(--pine-600); --accent-press: var(--pine-700);
  --accent-fg: var(--pine-600); --accent-subtle: var(--pine-100); --on-accent: #FFF8F0;
  --running: var(--pine-500); --waiting: #94650F; --done: #3E5F70; --error: #A3302A; --error-bg: #F2D7D0; --stopped: var(--graphite-500);
  --shadow-pop: 0 0 0 1px var(--border-2), 0 8px 24px rgba(60,40,20,.12);
  --ring: 0 0 0 2px var(--bg), 0 0 0 4px var(--accent);
`;
const dark = `
  color-scheme: dark;
  --bg: #201811; --bg-sunken: #18120D;
  --surface: #281E16; --surface-2: #31251B; --surface-3: #45362A;
  --fg-1: #F1E6D2; --fg-2: #CBBBA2; --fg-3: #A38F76; --fg-4: #7C6752;
  --border-1: #45362A; --border-2: #5B4838; --border-strong: #7C6752;
  --accent: var(--pine-400); --accent-hover: var(--pine-300); --accent-press: var(--pine-500);
  --accent-fg: var(--pine-300); --accent-subtle: var(--pine-900); --on-accent: #120D09;
  --running: var(--pine-300); --waiting: #D8A64E; --done: #9DB6C3; --error: #E0705E; --error-bg: #45190F; --stopped: #A38F76;
  --shadow-pop: 0 0 0 1px var(--border-2), 0 8px 24px rgba(0,0,0,.5);
  --ring: 0 0 0 2px var(--bg), 0 0 0 4px var(--accent);
`;

export const panelCSS = String.raw`
.cw-panel {
  --vellum-50:#FAF6EE;--vellum-100:#F4EEE2;--vellum-150:#EEE7D9;--vellum-200:#E7DECD;--vellum-300:#DACEB8;--vellum-400:#C9BAA0;
  --graphite-400:#978B7B;--graphite-500:#766C60;--graphite-600:#5C544B;--graphite-900:#2A2622;
  --pine-100:#D8E8DF;--pine-300:#86C4AB;--pine-400:#5FAE8F;--pine-500:#1F5E4A;--pine-600:#194D3D;--pine-700:#133D30;--pine-900:#12302A;
  ${light}
  --sans: 'IBM Plex Sans', system-ui, -apple-system, 'Segoe UI', sans-serif;
  --mono: 'IBM Plex Mono', ui-monospace, Menlo, monospace;
  --serif: 'Newsreader', Georgia, 'Times New Roman', serif;
  --ease-step: cubic-bezier(.2,0,0,1);
  --ease-out: cubic-bezier(.16,1,.3,1);
  --fast: 140ms; --base: 220ms;
  --hit: 32px;
  --pad: 16px;

  color-scheme: light;
  box-sizing: border-box;
  position: relative;
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 100%;
  min-height: 0;
  overflow: hidden;
  background: var(--bg);
  color: var(--fg-1);
  font: 400 13px/1.5 var(--sans);
  -webkit-font-smoothing: antialiased;
  -webkit-tap-highlight-color: transparent;
  text-align: left;
}
.cw-panel.cw-in-page { height: 100vh; height: 100dvh; }
.cw-panel[data-layout="sheet"] { --hit: 44px; font-size: 15px; }
@media (prefers-color-scheme: dark) { .cw-panel:not([data-theme="light"]) { ${dark} } }
.cw-panel[data-theme="dark"] { ${dark} }

.cw-panel *, .cw-panel *::before, .cw-panel *::after { box-sizing: border-box; }
.cw-panel :where(h1, h2, h3, p, ul, ol, blockquote) { margin: 0; }
.cw-panel :where(button, input, select, textarea) { font: inherit; color: inherit; letter-spacing: inherit; }
.cw-panel :where(button) { cursor: pointer; }
.cw-panel svg.cw-icon { width: 16px; height: 16px; flex: none; stroke-width: 1.5; }
.cw-panel .cw-sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
.cw-panel :focus-visible { outline: 0; box-shadow: var(--ring); }
.cw-panel [hidden] { display: none !important; }

/* buttons (the design system's Button and IconButton) */
.cw-panel .cw-btn {
  display: inline-flex; align-items: center; justify-content: center; gap: 8px;
  height: var(--hit); padding: 0 12px; border: 1px solid transparent; border-radius: 2px;
  background: transparent; color: var(--fg-2); font-size: 13px; font-weight: 500; line-height: 1; white-space: nowrap;
  transition: background var(--fast) var(--ease-step), border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step), opacity var(--fast) var(--ease-step);
  user-select: none;
}
.cw-panel[data-layout="sheet"] .cw-btn { font-size: 14px; }
.cw-panel .cw-btn:hover { background: var(--surface-2); color: var(--fg-1); }
.cw-panel .cw-btn:active:not(:disabled) { transform: translateY(1px); }
.cw-panel .cw-btn:disabled { opacity: .45; cursor: not-allowed; }
.cw-panel .cw-btn--primary { background: var(--accent); color: var(--on-accent); }
.cw-panel .cw-btn--primary:hover { background: var(--accent-hover); color: var(--on-accent); }
.cw-panel .cw-btn--primary:active { background: var(--accent-press); }
.cw-panel .cw-btn--secondary { border-color: var(--border-strong); color: var(--fg-1); }
.cw-panel .cw-btn--secondary:hover { border-color: var(--fg-1); }
.cw-panel .cw-btn--danger { color: var(--error); }
.cw-panel .cw-btn--danger:hover { background: var(--error-bg); color: var(--error); }
.cw-panel .cw-btn--icon { width: var(--hit); padding: 0; flex: none; }
.cw-panel .cw-btn[aria-pressed="true"], .cw-panel .cw-btn[aria-expanded="true"] { background: var(--surface-3); color: var(--fg-1); }
.cw-panel a.cw-btn { text-decoration: none; }

/* header: one row; the mark turns into a back arrow away from the list */
.cw-panel .cw-header {
  flex: none; display: flex; align-items: center; gap: 2px; height: 48px; padding: 0 6px 0 8px;
  border-bottom: 1px solid var(--border-1);
}
.cw-panel[data-layout="sheet"] .cw-header { height: 56px; }
.cw-panel .cw-mark { width: var(--hit); height: var(--hit); display: grid; place-items: center; color: var(--fg-1); flex: none; }
.cw-panel .cw-mark svg { width: 18px; height: 20px; }
.cw-panel .cw-heading { flex: 1; min-width: 0; padding-left: 4px; font-size: 13px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.cw-panel[data-layout="sheet"] .cw-heading { font-size: 15px; }
.cw-panel .cw-busy { position: relative; }
.cw-panel .cw-busy::after {
  content: ""; position: absolute; top: 7px; right: 7px; width: 6px; height: 6px; border-radius: 50%;
  background: var(--waiting); animation: cw-pulse 1.4s var(--ease-step) infinite;
}

/* main */
.cw-panel .cw-main { flex: 1; min-height: 0; overflow-y: auto; overscroll-behavior: contain; scrollbar-width: thin; scrollbar-color: var(--border-2) transparent; outline: 0; }
.cw-panel .cw-view { display: flex; flex-direction: column; min-height: 100%; }
.cw-panel .cw-view.is-new { animation: cw-in var(--base) var(--ease-out); }
@keyframes cw-in { from { opacity: 0; transform: translateY(3px); } }

/* list */
.cw-panel .cw-list { list-style: none; margin: 0; padding: 6px 6px 8px; display: flex; flex-direction: column; }
.cw-panel .cw-row {
  display: grid; grid-template-columns: 18px 1fr; column-gap: 10px; align-items: start;
  width: 100%; padding: 10px; border: 0; border-radius: 2px; background: transparent; text-align: left; color: inherit;
  transition: background var(--fast) var(--ease-step);
}
.cw-panel[data-layout="sheet"] .cw-row { padding: 12px 10px; }
.cw-panel .cw-row:hover { background: var(--surface-2); }
.cw-panel .cw-row-icon { padding-top: 3px; color: var(--fg-3); display: flex; }
.cw-panel .cw-row-text { min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.cw-panel .cw-row-original {
  font: 400 15px/1.35 var(--serif); color: var(--fg-1);
  overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical;
}
.cw-panel[data-layout="sheet"] .cw-row-original { font-size: 17px; }
.cw-panel .cw-row-sub { font-size: 12.5px; color: var(--fg-3); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.cw-panel[data-layout="sheet"] .cw-row-sub { font-size: 14px; }
.cw-panel .cw-row-sub.is-change { font: 400 15px/1.35 var(--serif); color: var(--accent-fg); white-space: normal; overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.cw-panel[data-layout="sheet"] .cw-row-sub.is-change { font-size: 17px; }
.cw-panel .cw-row.is-replaced .cw-row-original { color: var(--fg-3); text-decoration: line-through; text-decoration-color: var(--fg-4); -webkit-line-clamp: 1; }
.cw-panel .cw-row.is-quiet .cw-row-original { color: var(--fg-3); }
.cw-panel .cw-row[data-state="dismissed"] .cw-row-original { text-decoration: line-through; text-decoration-color: var(--fg-4); }

/* lifecycle: running = pine, waiting = ochre, done = slate, error = madder, stopped = graphite */
.cw-panel [data-state="asking"] > .cw-row-icon { color: var(--waiting); }
.cw-panel [data-state="suggested"] > .cw-row-icon { color: var(--fg-2); }
.cw-panel [data-state="previewing"] > .cw-row-icon { color: var(--running); }
.cw-panel [data-state="kept"] > .cw-row-icon { color: var(--done); }
.cw-panel [data-state="failed"] > .cw-row-icon { color: var(--error); }
.cw-panel [data-state="dismissed"] > .cw-row-icon { color: var(--fg-4); }
.cw-panel .cw-spin { animation: cw-spin 900ms linear infinite; }
@keyframes cw-spin { to { transform: rotate(360deg); } }
@keyframes cw-pulse { 50% { opacity: .35; } }

.cw-panel .cw-disclosure {
  display: flex; align-items: center; gap: 6px; margin: 4px 6px 0; padding: 0 10px; height: var(--hit); width: calc(100% - 12px);
  border: 0; border-top: 1px solid var(--border-1); border-radius: 0; background: none;
  color: var(--fg-3); font: 500 11px/1 var(--mono); letter-spacing: .08em;
}
.cw-panel .cw-disclosure:hover { color: var(--fg-1); }
.cw-panel .cw-disclosure .cw-chevron { transition: transform var(--fast) var(--ease-step); }
.cw-panel .cw-disclosure[aria-expanded="true"] .cw-chevron { transform: rotate(90deg); }
.cw-panel .cw-disclosure svg.cw-icon { width: 14px; height: 14px; }

.cw-panel .cw-hintrow {
  display: flex; align-items: center; gap: 10px; margin: 10px 12px 2px; padding: 0 12px; min-height: calc(var(--hit) + 8px); width: calc(100% - 24px);
  border: 1px dashed var(--border-2); border-radius: 2px; background: none; text-align: left; color: var(--fg-2); font-size: 13px;
  transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step);
}
.cw-panel .cw-hintrow:hover { border-color: var(--border-strong); color: var(--fg-1); }
.cw-panel .cw-hintrow svg.cw-icon { color: var(--accent-fg); }
.cw-panel .cw-hintrow span { flex: 1; }

.cw-panel .cw-empty {
  flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 10px;
  padding: 32px 24px; text-align: center; color: var(--fg-3);
}
.cw-panel .cw-empty-icon { width: 40px; height: 40px; border-radius: 50%; display: grid; place-items: center; border: 1px dashed var(--border-2); }
.cw-panel .cw-empty-icon svg.cw-icon { width: 18px; height: 18px; }
.cw-panel .cw-chips { display: flex; flex-direction: column; gap: 6px; width: 100%; max-width: 280px; margin-top: 6px; }
.cw-panel .cw-chip {
  min-height: var(--hit); padding: 6px 12px; border: 1px solid var(--border-1); border-radius: 2px;
  background: var(--surface); text-align: left; font-size: 13px; color: var(--fg-2);
  transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step);
}
.cw-panel .cw-chip:hover { border-color: var(--border-strong); color: var(--fg-1); }

/* detail */
.cw-panel .cw-detail { padding: 16px var(--pad) 20px; display: flex; flex-direction: column; gap: 12px; }
.cw-panel .cw-original {
  position: relative; padding: 2px 0 2px 12px; border-left: 2px solid var(--border-2);
  font: 400 18px/1.4 var(--serif); letter-spacing: -0.005em; color: var(--fg-1); overflow-wrap: anywhere;
  transition: color var(--base) var(--ease-step);
}
.cw-panel[data-layout="sheet"] .cw-original { font-size: 20px; }
.cw-panel .cw-original.is-replaced { color: var(--fg-3); text-decoration: line-through; text-decoration-color: var(--fg-4); text-decoration-thickness: 1px; }
.cw-panel .cw-original.is-missing { color: var(--fg-3); border-left-style: dashed; }
.cw-panel .cw-original .cw-missing { display: inline-flex; vertical-align: -2px; margin-left: 6px; color: var(--waiting); }

.cw-panel .cw-cards { display: flex; flex-direction: column; gap: 6px; }
.cw-panel .cw-card {
  position: relative; padding: 10px 12px;
  border: 1px solid var(--border-1); border-radius: 2px; background: var(--surface);
  transition: border-color var(--fast) var(--ease-step), background var(--fast) var(--ease-step);
}
.cw-panel .cw-card.is-new { animation: cw-in var(--base) var(--ease-out) both; }
.cw-panel .cw-card:hover { border-color: var(--border-2); }
.cw-panel .cw-card.is-chosen { background: var(--accent-subtle); border-color: var(--accent); }
.cw-panel .cw-card.is-kept { border-color: var(--done); }
.cw-panel .cw-card-pick { all: unset; display: block; cursor: pointer; border-radius: 2px; padding-right: 30px; }
.cw-panel .cw-card-pick:focus-visible { box-shadow: var(--ring); }
.cw-panel .cw-card-pick[aria-disabled="true"] { cursor: default; }
.cw-panel .cw-card-text { font: 400 16px/1.4 var(--serif); color: var(--fg-1); overflow-wrap: anywhere; }
.cw-panel[data-layout="sheet"] .cw-card-text { font-size: 18px; }
.cw-panel .cw-card-corner { position: absolute; top: 5px; right: 5px; display: flex; }
.cw-panel .cw-card-corner .cw-btn { height: 28px; width: 28px; color: var(--fg-3); }
.cw-panel[data-layout="sheet"] .cw-card-corner .cw-btn { height: 44px; width: 44px; margin: -6px -4px 0 0; }
.cw-panel .cw-reveal { opacity: 0; transition: opacity var(--fast) var(--ease-step); }
.cw-panel .cw-card:hover .cw-reveal, .cw-panel .cw-card:focus-within .cw-reveal { opacity: 1; }
@media (hover: none) { .cw-panel .cw-reveal { opacity: 1; } }
.cw-panel .cw-kept-mark { color: var(--done); padding: 6px; display: flex; }
/* the chosen card unfolds: why, then what to do */
.cw-panel .cw-fold { display: grid; grid-template-rows: 0fr; transition: grid-template-rows var(--base) var(--ease-out); }
.cw-panel .cw-fold > div { overflow: hidden; min-height: 0; }
.cw-panel .cw-card.is-chosen .cw-fold, .cw-panel .cw-card.is-kept .cw-fold { grid-template-rows: 1fr; }
.cw-panel .cw-why { padding-top: 4px; font-size: 12.5px; color: var(--fg-3); line-height: 1.45; }
.cw-panel[data-layout="sheet"] .cw-why { font-size: 14px; }
.cw-panel .cw-actions { display: flex; align-items: center; justify-content: flex-end; gap: 4px; padding-top: 8px; }
.cw-panel .cw-actions .cw-btn { height: 28px; }
.cw-panel[data-layout="sheet"] .cw-actions .cw-btn { height: var(--hit); }

.cw-panel .cw-tools { display: flex; gap: 2px; margin-left: -8px; }
.cw-panel .cw-tools .cw-btn { color: var(--fg-3); }
.cw-panel .cw-tools .cw-btn:hover { color: var(--fg-1); }

.cw-panel .cw-edit { display: flex; flex-direction: column; gap: 4px; padding: 8px 8px 6px; border: 1px solid var(--accent); border-radius: 2px; background: var(--surface); animation: cw-in var(--base) var(--ease-out); }
.cw-panel .cw-edit textarea { width: 100%; resize: none; border: 0; outline: 0; background: transparent; padding: 2px 4px; font: 400 16px/1.4 var(--serif); box-shadow: none; }
.cw-panel[data-layout="sheet"] .cw-edit textarea { font-size: 18px; }
.cw-panel .cw-edit .cw-actions { padding-top: 0; }

.cw-panel .cw-note {
  width: 100%; resize: none; padding: 6px 0; border: 0; border-bottom: 1px solid transparent; border-radius: 0; background: none; outline: 0;
  font-size: 13px; line-height: 1.5; color: var(--fg-2); field-sizing: content; min-height: 32px; max-height: 160px;
  transition: border-color var(--fast) var(--ease-step);
}
.cw-panel[data-layout="sheet"] .cw-note { font-size: 15px; min-height: 44px; }
.cw-panel .cw-note::placeholder { color: var(--fg-4); }
.cw-panel .cw-note:hover { border-bottom-color: var(--border-1); }
.cw-panel .cw-note:focus { border-bottom-color: var(--accent); color: var(--fg-1); box-shadow: none; }

.cw-panel .cw-error { display: flex; align-items: center; gap: 8px; padding: 4px 4px 4px 10px; border-left: 2px solid var(--error); background: var(--error-bg); font-size: 13px; color: var(--fg-1); }
.cw-panel .cw-error > svg.cw-icon { color: var(--error); }
.cw-panel .cw-error span { flex: 1; min-width: 0; padding: 4px 0; }

/* thread */
.cw-panel .cw-thread { display: flex; flex-direction: column; gap: 12px; padding-top: 4px; }
.cw-panel .cw-msg { display: flex; flex-direction: column; }
.cw-panel .cw-msg.is-new { animation: cw-in var(--base) var(--ease-out) both; }
.cw-panel .cw-msg-body { font-size: 13.5px; line-height: 1.55; overflow-wrap: anywhere; }
.cw-panel[data-layout="sheet"] .cw-msg-body { font-size: 15px; }
.cw-panel .cw-msg--user { align-items: flex-end; }
.cw-panel .cw-msg--user .cw-msg-body { max-width: 85%; padding: 7px 11px; border-radius: 2px; background: var(--surface-2); }
.cw-panel .cw-msg-body p + p, .cw-panel .cw-msg-body p + ul, .cw-panel .cw-msg-body ul + p { margin-top: 8px; }
.cw-panel .cw-msg-body ul { margin: 0; padding-left: 18px; }
.cw-panel .cw-msg-body code { font: 12px/1.4 var(--mono); padding: 1px 4px; border-radius: 1px; background: var(--surface-2); }
.cw-panel .cw-msg-body strong { font-weight: 600; }
.cw-panel .cw-dots { display: inline-flex; gap: 4px; padding: 8px 0; color: var(--fg-3); }
.cw-panel .cw-dots i { width: 5px; height: 5px; border-radius: 50%; background: currentColor; opacity: .25; animation: cw-dot 1.2s infinite var(--ease-step); }
.cw-panel .cw-dots i:nth-child(2) { animation-delay: .15s; }
.cw-panel .cw-dots i:nth-child(3) { animation-delay: .3s; }
@keyframes cw-dot { 0%, 60%, 100% { opacity: .25; transform: none; } 30% { opacity: 1; transform: translateY(-2px); } }
.cw-panel .cw-chat { flex: 1; display: flex; flex-direction: column; padding: 12px var(--pad) 16px; }
.cw-panel .cw-chat > .cw-empty { padding: 24px 8px; }

/* menus: the design system's loupe level, the only shadow */
.cw-panel .cw-anchor { position: relative; display: flex; }
.cw-panel .cw-menu {
  position: absolute; top: calc(100% + 4px); right: 0; z-index: 20; min-width: 224px; padding: 4px;
  background: var(--surface); border-radius: 2px; box-shadow: var(--shadow-pop);
  display: flex; flex-direction: column; gap: 1px; animation: cw-in var(--fast) var(--ease-out);
}
.cw-panel .cw-menu-item {
  display: flex; align-items: center; gap: 10px; width: 100%; min-height: var(--hit); padding: 0 10px;
  border: 0; border-radius: 2px; background: none; text-align: left; font-size: 13px; color: var(--fg-1);
}
.cw-panel[data-layout="sheet"] .cw-menu-item { font-size: 15px; }
.cw-panel .cw-menu-item:hover, .cw-panel .cw-menu-item:focus-visible { background: var(--surface-2); box-shadow: none; }
.cw-panel .cw-menu-item svg.cw-icon { color: var(--fg-3); }
.cw-panel .cw-menu-item.cw-btn--danger, .cw-panel .cw-menu-item.cw-btn--danger svg.cw-icon { color: var(--error); }
.cw-panel .cw-menu-item:disabled { opacity: .45; cursor: not-allowed; }
.cw-panel .cw-menu-sep { height: 1px; background: var(--border-1); margin: 3px -4px; }
.cw-panel .cw-scope { display: flex; padding: 2px; gap: 2px; margin-bottom: 3px; background: var(--bg-sunken); border-radius: 2px; }
.cw-panel .cw-scope button {
  flex: 1; display: inline-flex; align-items: center; justify-content: center; gap: 6px; height: 28px;
  border: 0; border-radius: 2px; background: none; font-size: 12px; font-weight: 500; color: var(--fg-3);
}
.cw-panel[data-layout="sheet"] .cw-scope button { height: 44px; font-size: 14px; }
.cw-panel .cw-scope button[aria-pressed="true"] { background: var(--surface); color: var(--fg-1); box-shadow: 0 0 0 1px var(--border-1); }
.cw-panel .cw-count { font: 500 11px/1 var(--mono); color: var(--fg-3); }

/* selection banner, above the composer: in thumb reach on a phone */
.cw-panel .cw-banner {
  display: flex; align-items: center; gap: 8px; margin: 0 8px 6px; padding: 4px 4px 4px 12px;
  border-left: 2px solid var(--accent); background: var(--surface);
  animation: cw-rise var(--base) var(--ease-out);
}
@keyframes cw-rise { from { opacity: 0; transform: translateY(6px); } }
.cw-panel .cw-banner-quote {
  flex: 1; min-width: 0; font: 400 14px/1.35 var(--serif); color: var(--fg-2);
  overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical;
}
.cw-panel[data-layout="sheet"] .cw-banner-quote { font-size: 16px; }

/* composer */
.cw-panel .cw-dock { flex: none; padding: 8px 0; padding-bottom: max(8px, env(safe-area-inset-bottom)); border-top: 1px solid var(--border-1); }
.cw-panel .cw-composer {
  display: flex; align-items: flex-end; gap: 2px; margin: 0 8px; padding: 3px;
  border: 1px solid var(--border-2); border-radius: 2px; background: var(--surface);
  transition: border-color var(--fast) var(--ease-step);
}
.cw-panel .cw-composer:focus-within { border-color: var(--accent); }
.cw-panel .cw-composer.is-recording { border-color: var(--error); }
.cw-panel .cw-composer textarea {
  flex: 1; min-width: 0; min-height: var(--hit); max-height: 168px; padding: 7px 8px 6px; border: 0; outline: 0; resize: none;
  background: transparent; font-size: 13.5px; line-height: 1.4; box-shadow: none;
}
.cw-panel[data-layout="sheet"] .cw-composer textarea { font-size: 16px; padding-top: 11px; }
.cw-panel .cw-composer textarea::placeholder { color: var(--fg-4); }
.cw-panel .cw-rec { flex: 1; display: flex; align-items: center; gap: 8px; min-height: var(--hit); padding: 0 8px; font: 500 12px/1 var(--mono); color: var(--fg-2); font-variant-numeric: tabular-nums; }
.cw-panel .cw-rec-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--error); animation: cw-pulse 1s infinite; }
.cw-panel .cw-mic { touch-action: none; user-select: none; -webkit-user-select: none; }
.cw-panel .cw-mic[aria-pressed="true"] { background: var(--error); color: #FFF8F0; }
.cw-panel .cw-send { background: var(--accent); color: var(--on-accent); }
.cw-panel .cw-send:hover { background: var(--accent-hover); color: var(--on-accent); }
.cw-panel .cw-send:disabled { background: transparent; color: var(--fg-4); opacity: 1; }

/* settings */
.cw-panel .cw-settings { padding: 16px var(--pad) 24px; display: flex; flex-direction: column; gap: 24px; }
.cw-panel .cw-group { display: flex; flex-direction: column; gap: 8px; }
.cw-panel .cw-label { font: 500 11px/1 var(--mono); letter-spacing: .12em; text-transform: uppercase; color: var(--fg-3); display: flex; align-items: center; gap: 6px; min-height: 16px; }
.cw-panel .cw-label .cw-ok { color: var(--running); display: inline-flex; }
.cw-panel .cw-label .cw-ok svg.cw-icon { width: 14px; height: 14px; }
.cw-panel .cw-control {
  position: relative; display: flex; align-items: center; height: calc(var(--hit) + 4px);
  border: 1px solid var(--border-2); border-radius: 2px; background: var(--bg-sunken);
  transition: border-color var(--fast) var(--ease-step);
}
.cw-panel .cw-control:hover { border-color: var(--border-strong); }
.cw-panel .cw-control:focus-within { border-color: var(--accent); }
.cw-panel .cw-control :is(input, select) { flex: 1; min-width: 0; height: 100%; padding: 0 10px; border: 0; outline: 0; background: none; font-size: 13.5px; box-shadow: none; }
.cw-panel[data-layout="sheet"] .cw-control :is(input, select) { font-size: 16px; }
.cw-panel .cw-control select { appearance: none; cursor: pointer; padding-right: 30px; }
.cw-panel .cw-control select option { background: var(--surface-2); color: var(--fg-1); }
.cw-panel .cw-caret { position: absolute; right: 9px; pointer-events: none; color: var(--fg-3); }
.cw-panel .cw-control input.cw-key { font-family: var(--mono); font-size: 12.5px; }
.cw-panel .cw-control input.cw-key::placeholder { font-family: var(--sans); font-size: 13.5px; color: var(--fg-4); }
.cw-panel .cw-control .cw-btn { height: calc(var(--hit) - 4px); width: calc(var(--hit) - 4px); margin-right: 2px; }
.cw-panel .cw-split { display: flex; gap: 4px; }
.cw-panel .cw-split .cw-control { flex: 1; }
.cw-panel .cw-split > .cw-btn { height: calc(var(--hit) + 4px); width: calc(var(--hit) + 4px); }
.cw-panel .cw-textarea {
  width: 100%; min-height: 150px; padding: 8px 10px; resize: vertical; outline: 0;
  border: 1px solid var(--border-2); border-radius: 2px; background: var(--bg-sunken); font-size: 13px; line-height: 1.5;
}
.cw-panel .cw-textarea:focus { border-color: var(--accent); box-shadow: none; }
.cw-panel .cw-check { display: inline-flex; align-items: center; gap: 8px; min-height: var(--hit); font-size: 13px; color: var(--fg-1); cursor: pointer; align-self: flex-start; }
.cw-panel[data-layout="sheet"] .cw-check { font-size: 15px; }
.cw-panel .cw-check input { width: 16px; height: 16px; margin: 0; accent-color: var(--accent); }
.cw-panel .cw-fine { font-size: 12px; color: var(--fg-3); }
.cw-panel[data-layout="sheet"] .cw-fine { font-size: 13.5px; }
.cw-panel .cw-fine a { color: var(--accent-fg); text-underline-offset: 2px; }
.cw-panel .cw-buttons { display: flex; align-items: center; gap: 4px; }
.cw-panel .cw-buttons:not(:has(> .cw-btn:not([hidden]))) { display: none; }
.cw-panel .cw-buttons > .cw-btn { animation: cw-in var(--fast) var(--ease-out); }
.cw-panel .cw-buttons .cw-grow { flex: 1; }

/* booting */
.cw-panel .cw-skeleton { padding: 18px var(--pad); display: flex; flex-direction: column; gap: 14px; }
.cw-panel .cw-skeleton i { display: block; height: 10px; border-radius: 1px; background: var(--surface-2); animation: cw-pulse 1.4s infinite; }

/* toasts */
.cw-panel .cw-toasts { position: absolute; left: 0; right: 0; top: 56px; z-index: 30; display: flex; flex-direction: column; align-items: center; gap: 6px; padding: 0 12px; pointer-events: none; }
.cw-panel .cw-toast {
  pointer-events: auto; display: flex; align-items: center; gap: 8px; max-width: 100%; padding: 6px 6px 6px 12px;
  border-radius: 2px; background: var(--fg-1); color: var(--bg); font-size: 13px; box-shadow: var(--shadow-pop);
  animation: cw-in var(--base) var(--ease-out);
}
.cw-panel .cw-toast .cw-btn { height: 24px; width: 24px; color: inherit; opacity: .7; }
.cw-panel .cw-toast .cw-btn:hover { background: none; opacity: 1; color: inherit; }
.cw-panel .cw-toast--error > svg.cw-icon { color: #E0705E; }
.cw-panel .cw-toast--ok > svg.cw-icon { color: var(--pine-300); }

@media (prefers-reduced-motion: reduce) {
  .cw-panel *, .cw-panel *::before, .cw-panel *::after { animation-duration: 1ms !important; animation-iteration-count: 1 !important; transition-duration: 1ms !important; }
}
`;
