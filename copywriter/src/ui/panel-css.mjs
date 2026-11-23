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
  width: 100%; resize: none; padding: 2px 0; margin-top: -6px; border: 0; border-bottom: 1px solid transparent; border-radius: 0; background: none; outline: 0;
  font-size: 12.5px; line-height: 1.5; color: var(--fg-3); field-sizing: content; min-height: 28px; max-height: 160px;
  transition: border-color var(--fast) var(--ease-step);
}
.cw-panel[data-layout="sheet"] .cw-note { font-size: 14px; min-height: 36px; }
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
.cw-panel .cw-thread > .cw-cards { margin: 0; }
/* a page chat's reply: the annotations it made, each a link to open */
.cw-panel .cw-links { display: flex; flex-direction: column; gap: 6px; margin-top: 6px; }
.cw-panel .cw-msg-body + .cw-links { margin-top: 8px; }
.cw-panel .cw-link {
  display: flex; flex-direction: column; align-items: flex-start; gap: 2px; width: 100%; min-height: var(--hit); padding: 7px 10px;
  border: 1px solid var(--border-1); border-left: 2px solid var(--accent); border-radius: 2px; background: var(--surface);
  text-align: left; color: var(--fg-1); cursor: pointer; transition: border-color var(--fast) var(--ease-step);
}
.cw-panel .cw-link:hover { border-color: var(--border-2); border-left-color: var(--accent); }
.cw-panel .cw-link-from { font-size: 12.5px; color: var(--fg-3); text-decoration: line-through; text-decoration-color: var(--fg-4);
  overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.cw-panel .cw-link-arrow { display: none; }
.cw-panel .cw-link-to { font: 400 15px/1.35 var(--serif); }
.cw-panel[data-layout="sheet"] .cw-link-from { font-size: 14px; }
.cw-panel[data-layout="sheet"] .cw-link-to { font-size: 17px; }
/* a spoken message: a small mic before its words */
.cw-panel .cw-spoken { display: inline-flex; vertical-align: -2px; margin-right: 6px; color: var(--fg-4); }
.cw-panel .cw-spoken svg.cw-icon { width: 13px; height: 13px; }
.cw-panel .cw-msg-body.is-spoken > p:first-of-type { display: inline; }
.cw-panel .cw-msg-body.is-spoken .cw-dots { padding: 2px 0; vertical-align: middle; }
/* a reply's thinking: quiet until opened */
.cw-panel .cw-thought { margin-bottom: 4px; }
.cw-panel .cw-thought > summary {
  display: inline-flex; align-items: center; gap: 4px; min-height: 24px; list-style: none; cursor: pointer;
  font-size: 12px; color: var(--fg-4); transition: color var(--fast) var(--ease-step);
}
.cw-panel .cw-thought > summary::-webkit-details-marker { display: none; }
.cw-panel .cw-thought > summary:hover { color: var(--fg-2); }
.cw-panel .cw-thought > summary .cw-chevron { width: 12px; height: 12px; transition: transform var(--fast) var(--ease-step); }
.cw-panel .cw-thought[open] > summary .cw-chevron { transform: rotate(90deg); }
.cw-panel .cw-thought-body {
  margin: 2px 0 6px 5px; padding-left: 10px; border-left: 1px solid var(--border-1);
  font-size: 12.5px; line-height: 1.5; color: var(--fg-3); animation: cw-in var(--fast) var(--ease-out);
}
.cw-panel[data-layout="sheet"] .cw-thought > summary { font-size: 13px; min-height: 32px; }
.cw-panel[data-layout="sheet"] .cw-thought-body { font-size: 14px; }
.cw-panel .cw-thought-body p + p { margin-top: 6px; }
/* an audience the model saved or chose: a chip that opens it */
.cw-panel .cw-audience-chips { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; }
.cw-panel .cw-audience-chip {
  display: inline-flex; align-items: center; gap: 6px; min-height: 28px; padding: 0 10px; max-width: 100%;
  border: 1px solid var(--border-1); border-radius: 999px; background: var(--surface);
  font-size: 12.5px; color: var(--fg-2); cursor: pointer; transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step);
}
.cw-panel .cw-audience-chip span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cw-panel .cw-audience-chip svg.cw-icon { width: 14px; height: 14px; color: var(--fg-3); }
.cw-panel .cw-audience-chip:hover { border-color: var(--border-2); color: var(--fg-1); }
.cw-panel .cw-audience-chip.is-active { border-color: var(--accent); color: var(--accent-fg); }
.cw-panel .cw-audience-chip.is-active svg.cw-icon { color: var(--accent-fg); }
.cw-panel .cw-audience-chip:disabled { opacity: .5; cursor: default; }
.cw-panel[data-layout="sheet"] .cw-audience-chip { min-height: 36px; font-size: 14px; }
/* a short word under the thread: nothing heard */
.cw-panel .cw-hint { padding: 6px 2px 0; font-size: 12.5px; color: var(--fg-3); animation: cw-in var(--fast) var(--ease-out); }
/* the thinking level: a segmented control under the model */
.cw-panel .cw-thinking { margin: 0; }
.cw-panel .cw-dots { display: inline-flex; gap: 4px; padding: 8px 0; color: var(--fg-3); }
.cw-panel .cw-dots i { width: 5px; height: 5px; border-radius: 50%; background: currentColor; opacity: .25; animation: cw-dot 1.2s infinite var(--ease-step); }
.cw-panel .cw-dots i:nth-child(2) { animation-delay: .15s; }
.cw-panel .cw-dots i:nth-child(3) { animation-delay: .3s; }
@keyframes cw-dot { 0%, 60%, 100% { opacity: .25; transform: none; } 30% { opacity: 1; transform: translateY(-2px); } }
.cw-panel .cw-chat { flex: 1; display: flex; flex-direction: column; padding: 12px var(--pad) 16px; }
.cw-panel .cw-chat > .cw-empty { padding: 24px 8px; }
/* the page's earlier conversations, under an empty one */
.cw-panel .cw-conversations { list-style: none; margin: 8px 0 0; padding: 0; display: flex; flex-direction: column; }
.cw-panel .cw-conversation {
  display: flex; align-items: baseline; gap: 12px; width: 100%; min-height: var(--hit); padding: 6px 8px; border: 0; border-radius: 2px;
  background: none; text-align: left; color: var(--fg-2); cursor: pointer; transition: background var(--fast) var(--ease-step);
}
.cw-panel .cw-conversation:hover { background: var(--surface-2); color: var(--fg-1); }
.cw-panel .cw-conversation-title { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 13px; }
.cw-panel .cw-conversation-at { flex: none; font: 500 11px/1 var(--mono); color: var(--fg-4); }
.cw-panel[data-layout="sheet"] .cw-conversation { min-height: 44px; }
.cw-panel[data-layout="sheet"] .cw-conversation-title { font-size: 15px; }

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
/* a selection too long for one annotation: the hint shows, then goes */
.cw-panel .cw-banner--hint { max-height: 96px; overflow: hidden; padding: 8px 12px; font-size: 13px; color: var(--fg-2); border-left-color: var(--border-2);
  animation: cw-rise var(--base) var(--ease-out), cw-leave 400ms var(--ease-out) 4s forwards; }
.cw-panel[data-layout="sheet"] .cw-banner--hint { font-size: 15px; }
.cw-panel .cw-banner--hint svg.cw-icon { color: var(--fg-3); flex: none; }
@keyframes cw-leave { to { opacity: 0; max-height: 0; padding-top: 0; padding-bottom: 0; margin-bottom: 0; visibility: hidden; } }

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
.cw-panel .cw-pick[aria-pressed="true"] { background: var(--accent); color: var(--on-accent); }
.cw-panel .cw-pick[aria-pressed="true"]:hover { background: var(--accent-hover); }
.cw-panel button.cw-empty-icon { padding: 0; background: none; color: var(--fg-2); cursor: pointer; transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step); }
.cw-panel button.cw-empty-icon:hover { border-color: var(--accent); color: var(--accent-fg); }
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
/* a switch: a checkbox drawn as a track and a knob */
.cw-panel .cw-switch {
  appearance: none; flex: none; position: relative; width: 30px; height: 18px; margin: 0; border-radius: 9px; cursor: pointer;
  background: var(--border-2); transition: background var(--fast) var(--ease-step);
}
.cw-panel .cw-switch::after {
  content: ""; position: absolute; top: 2px; left: 2px; width: 14px; height: 14px; border-radius: 50%; background: var(--surface);
  transition: transform var(--fast) var(--ease-step);
}
.cw-panel .cw-switch:checked { background: var(--accent); }
.cw-panel .cw-switch:checked::after { transform: translateX(12px); }
.cw-panel .cw-switch:focus-visible { box-shadow: var(--ring); }
/* the audiences: one row each, its editor folded under it */
.cw-panel .cw-audiences { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; border-top: 1px solid var(--border-1); }
.cw-panel .cw-audiences > li { border-bottom: 1px solid var(--border-1); }
.cw-panel .cw-audience-head { display: flex; align-items: center; gap: 10px; min-height: calc(var(--hit) + 8px); }
.cw-panel[data-layout="sheet"] .cw-audience-head { min-height: 48px; }
.cw-panel .cw-audience-name {
  flex: 1; min-width: 0; display: flex; align-items: center; gap: 6px; min-height: var(--hit); padding: 0; border: 0; background: none;
  text-align: left; font-size: 13.5px; color: var(--fg-1); cursor: pointer;
}
.cw-panel[data-layout="sheet"] .cw-audience-name { font-size: 15px; }
.cw-panel .cw-audience-name > span:first-child { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cw-panel .cw-audience-name .cw-ok { color: var(--running); display: inline-flex; }
.cw-panel .cw-audience-name svg.cw-icon { width: 14px; height: 14px; }
.cw-panel .cw-audience-name .cw-chevron { color: var(--fg-4); transition: transform var(--fast) var(--ease-step); }
.cw-panel .cw-audience-name[aria-expanded="true"] .cw-chevron { transform: rotate(90deg); }
.cw-panel .cw-audience-edit { display: flex; flex-direction: column; gap: 6px; padding: 2px 0 12px 40px; animation: cw-in var(--fast) var(--ease-out); }
.cw-panel .cw-audiences li:has(> .cw-audience-edit) .cw-audience-edit { padding-left: 0; padding-top: 12px; }
.cw-panel .cw-input {
  width: 100%; height: calc(var(--hit) + 4px); padding: 0 10px; outline: 0; border: 1px solid var(--border-2); border-radius: 2px;
  background: var(--bg-sunken); font-size: 13.5px; color: var(--fg-1);
}
.cw-panel[data-layout="sheet"] .cw-input { font-size: 16px; }
.cw-panel .cw-input:focus { border-color: var(--accent); box-shadow: none; }
.cw-panel .cw-textarea--small { min-height: 0; }
/* a switch and its name, one row each; the tools sit under the length */
.cw-panel .cw-tools-group { gap: 0; margin-top: -14px; }
.cw-panel .cw-switch-row { display: flex; align-items: center; gap: 10px; min-height: var(--hit); font-size: 13.5px; color: var(--fg-1); cursor: pointer; }
.cw-panel[data-layout="sheet"] .cw-switch-row { min-height: 44px; font-size: 15px; }
/* a grounded answer's sources: small numbered links */
.cw-panel .cw-sources { list-style: none; margin: 8px 0 0; padding: 0; display: flex; flex-wrap: wrap; gap: 4px 6px; }
.cw-panel .cw-sources a {
  display: inline-flex; align-items: center; gap: 5px; max-width: 220px; min-height: 24px; padding: 0 8px 0 3px;
  border: 1px solid var(--border-1); border-radius: 999px; background: var(--surface);
  font-size: 12px; color: var(--fg-2); text-decoration: none; transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step);
}
.cw-panel .cw-sources a > span:last-child { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.cw-panel .cw-sources a:hover { border-color: var(--accent); color: var(--accent-fg); }
.cw-panel .cw-source-n { display: grid; place-items: center; min-width: 18px; height: 18px; border-radius: 50%; background: var(--surface-2); font: 500 10.5px/1 var(--mono); color: var(--fg-3); }
.cw-panel[data-layout="sheet"] .cw-sources a { min-height: 32px; font-size: 13px; }
.cw-panel .cw-field { display: flex; flex-direction: column; gap: 3px; }
.cw-panel .cw-field-label { font-size: 11.5px; color: var(--fg-4); }
.cw-panel[data-layout="sheet"] .cw-field-label { font-size: 12.5px; }
.cw-panel .cw-textarea::placeholder, .cw-panel .cw-input::placeholder { color: var(--fg-4); }
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
.cw-panel .cw-toasts { position: absolute; left: 0; right: 0; bottom: 12px; z-index: 30; display: flex; flex-direction: column; align-items: center; gap: 6px; padding: 0 12px; pointer-events: none; }
.cw-panel .cw-toast {
  pointer-events: auto; display: flex; align-items: center; gap: 8px; max-width: 100%; padding: 6px 6px 6px 12px;
  border-radius: 2px; background: var(--fg-1); color: var(--bg); font-size: 13px; box-shadow: var(--shadow-pop);
  animation: cw-in var(--base) var(--ease-out);
}
.cw-panel .cw-toast .cw-btn { height: 24px; width: 24px; color: inherit; opacity: .7; }
.cw-panel .cw-toast .cw-btn:hover { background: none; opacity: 1; color: inherit; }
.cw-panel .cw-toast--error > svg.cw-icon { color: #E0705E; }
.cw-panel .cw-toast--ok > svg.cw-icon { color: var(--pine-300); }

/* reading as an audience: the list's strip of audience chips */
.cw-panel .cw-strip { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; padding: 10px 12px 2px; }
.cw-panel .cw-strip-chip {
  display: inline-flex; align-items: center; gap: 5px; height: 28px; padding: 0 8px 0 3px;
  border: 1px solid var(--border-1); border-radius: 999px; background: var(--surface); color: var(--fg-2);
  font: 500 11px/1 var(--mono); transition: border-color var(--fast) var(--ease-step), color var(--fast) var(--ease-step);
}
.cw-panel .cw-strip-chip:not(:has(> :nth-child(2))) { padding-right: 3px; }
.cw-panel[data-layout="sheet"] .cw-strip-chip { height: 44px; padding-left: 6px; padding-right: 12px; font-size: 13px; }
.cw-panel .cw-strip-chip:hover, .cw-panel .cw-strip-chip[aria-expanded="true"] { border-color: var(--border-strong); color: var(--fg-1); }
.cw-panel .cw-strip-chip.is-active { border-color: var(--accent); }
.cw-panel .cw-strip-chip svg.cw-icon { width: 13px; height: 13px; }
.cw-panel .cw-strip-chip[data-reading="reading"] svg.cw-icon { color: var(--waiting); }
.cw-panel .cw-strip-chip[data-reading="failed"] svg.cw-icon { color: var(--error); }
.cw-panel .cw-avatar {
  display: inline-grid; place-items: center; width: 22px; height: 22px; border-radius: 50%;
  background: var(--surface-3); color: var(--fg-1); font: 600 9.5px/1 var(--sans); letter-spacing: .02em;
}
.cw-panel[data-layout="sheet"] .cw-avatar { width: 30px; height: 30px; font-size: 11px; }
.cw-panel .cw-strip-chip.is-active .cw-avatar { background: var(--accent-subtle); color: var(--accent-fg); }
.cw-panel .cw-cover { font: 500 11px/1 var(--mono); color: inherit; font-variant-numeric: tabular-nums; }
.cw-panel .cw-strip-chip[data-reading="done"] .cw-cover { animation: cw-in var(--base) var(--ease-out); }
.cw-panel .cw-strip-all { margin-left: auto; }
.cw-panel .cw-menu--left { right: auto; left: 0; }
.cw-panel .cw-menu-title { padding: 6px 10px 4px; font-size: 12px; color: var(--fg-3); max-width: 260px; }

/* the page's audience suggestion: the avatars, the suggested ones ticked; quiet, one row */
.cw-panel .cw-suggest {
  display: flex; width: fit-content; max-width: calc(100% - 24px); box-sizing: border-box; align-items: center; gap: 2px; margin: 10px 12px 0; padding: 2px 2px 2px 10px;
  border: 1px dashed var(--border-2); border-radius: 999px; animation: cw-in var(--base) var(--ease-out);
}
.cw-panel .cw-strip > .cw-suggest { margin: 0; }
.cw-panel .cw-suggest-mark { display: flex; color: var(--accent-fg); margin-right: 2px; }
.cw-panel .cw-suggest-mark svg.cw-icon { width: 13px; height: 13px; }
.cw-panel .cw-suggest-chip { all: unset; display: inline-grid; place-items: center; width: 28px; height: 28px; border-radius: 50%; cursor: pointer; }
.cw-panel[data-layout="sheet"] .cw-suggest-chip { width: 40px; height: 40px; }
.cw-panel .cw-suggest-chip:focus-visible { box-shadow: var(--ring); }
.cw-panel .cw-suggest-chip .cw-avatar { opacity: .45; transition: opacity var(--fast) var(--ease-step), background var(--fast) var(--ease-step), box-shadow var(--fast) var(--ease-step); }
.cw-panel .cw-suggest-chip.is-on .cw-avatar { opacity: 1; background: var(--accent-subtle); color: var(--accent-fg); box-shadow: 0 0 0 1.5px var(--accent); }
.cw-panel .cw-suggest .cw-btn { height: 28px; width: 28px; }
.cw-panel[data-layout="sheet"] .cw-suggest .cw-btn { height: 40px; width: 40px; }
.cw-panel .cw-suggest-ok:not(:disabled) { color: var(--accent-fg); }

/* rewrites side by side: each card for one audience carries its avatar */
.cw-panel .cw-card.is-for .cw-card-who { position: absolute; top: 9px; left: 10px; width: 20px; height: 20px; font-size: 8.5px; background: var(--accent-subtle); color: var(--accent-fg); }
.cw-panel .cw-card.is-for:has(> .cw-card-who) .cw-card-pick { padding-left: 28px; }
.cw-panel[data-layout="sheet"] .cw-card.is-for .cw-card-who { top: 11px; width: 24px; height: 24px; font-size: 9.5px; }
.cw-panel[data-layout="sheet"] .cw-card.is-for:has(> .cw-card-who) .cw-card-pick { padding-left: 32px; }
/* on a phone: a row to swipe, a card at a time with the next one peeking */
.cw-panel[data-layout="sheet"] .cw-cards--compare {
  flex-direction: row; align-items: flex-start; gap: 8px; overflow-x: auto; overscroll-behavior-x: contain;
  scroll-snap-type: x mandatory; scrollbar-width: none; margin: 0 calc(-1 * var(--pad)); padding: 0 var(--pad) 2px;
  scroll-padding-inline: var(--pad);
}
.cw-panel[data-layout="sheet"] .cw-cards--compare::-webkit-scrollbar { display: none; }
.cw-panel[data-layout="sheet"] .cw-cards--compare > .cw-card { flex: 0 0 84%; scroll-snap-align: start; }

/* a reading under way: its seconds */
.cw-panel .cw-elapsed { font: 500 11.5px/1 var(--mono); font-variant-numeric: tabular-nums; color: var(--waiting); }

/* a finding: its kind as the row's icon, the audience's initials on it */
.cw-panel .cw-row-icon.is-finding { position: relative; }
.cw-panel .cw-initials {
  position: absolute; right: -7px; bottom: -6px; min-width: 13px; height: 13px; padding: 0 2px; border-radius: 7px;
  display: grid; place-items: center; background: var(--surface-3); color: var(--fg-2); font: 600 7.5px/1 var(--sans);
  box-shadow: 0 0 0 2px var(--bg);
}
.cw-panel .cw-row.is-page .cw-row-original { font-family: var(--sans); font-size: 13.5px; font-style: italic; color: var(--fg-2); }
.cw-panel[data-layout="sheet"] .cw-row.is-page .cw-row-original { font-size: 15px; }
.cw-panel .cw-original.is-page { font-family: var(--sans); font-style: italic; border-left-style: dashed; }
.cw-panel .cw-original .cw-add { display: inline-flex; vertical-align: -2px; margin-right: 6px; color: var(--accent-fg); }
.cw-panel .cw-finding { display: flex; gap: 10px; align-items: flex-start; font-size: 13px; color: var(--fg-2); line-height: 1.45; }
.cw-panel[data-layout="sheet"] .cw-finding { font-size: 15px; }
.cw-panel .cw-finding-mark { position: relative; display: flex; padding-top: 2px; color: var(--waiting); }
.cw-panel .cw-finding-who {
  display: inline; margin-left: 6px; padding: 0; border: 0; background: none; color: var(--fg-3); font-size: .92em;
  text-decoration: underline dotted var(--fg-4); text-underline-offset: 3px;
}
.cw-panel .cw-finding-who:hover { color: var(--fg-1); }

/* the checklist */
.cw-panel .cw-reading { padding: 12px var(--pad) 20px; display: flex; flex-direction: column; gap: 8px; }
.cw-panel .cw-reading-head { display: flex; align-items: center; gap: 10px; min-height: var(--hit); color: var(--fg-3); font-size: 12.5px; }
.cw-panel .cw-cover--big { font-size: 15px; color: var(--fg-1); }
.cw-panel .cw-reading .cw-label { margin-top: 10px; }
.cw-panel .cw-checklist { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; }
.cw-panel .cw-check-item { display: flex; flex-wrap: wrap; align-items: flex-start; border-radius: 2px; transition: background var(--fast) var(--ease-step); }
.cw-panel .cw-check-item:hover { background: var(--surface-2); }
.cw-panel .cw-check-line {
  display: grid; grid-template-columns: 16px 1fr; gap: 10px; align-items: start; min-height: var(--hit);
  padding: 6px 6px; border: 0; background: none; text-align: left; color: var(--fg-1); font-size: 13px; line-height: 1.4;
}
.cw-panel[data-layout="sheet"] .cw-check-line { font-size: 15px; padding: 10px 6px; }
.cw-panel .cw-check-glyph { display: flex; padding-top: 1px; }
.cw-panel [data-status="answered"] .cw-check-glyph { color: var(--running); }
.cw-panel [data-status="partial"] .cw-check-glyph { color: var(--waiting); }
.cw-panel [data-status="missing"] .cw-check-glyph { color: var(--fg-4); }
.cw-panel [data-status="missing"] .cw-check-text { color: var(--fg-2); }
.cw-panel .cw-check-text { overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.cw-panel .cw-check-item.is-open .cw-check-text { -webkit-line-clamp: unset; }
.cw-panel .cw-check-act { opacity: 0; transition: opacity var(--fast) var(--ease-step); color: var(--fg-3); }
.cw-panel .cw-check-item:hover .cw-check-act, .cw-panel .cw-check-act:focus-visible, .cw-panel .cw-check-item.is-open .cw-check-act, .cw-panel .cw-check-act.is-linked { opacity: 1; }
@media (hover: none) { .cw-panel .cw-check-act { opacity: .55; } }
.cw-panel .cw-check-act.is-linked { color: var(--accent-fg); }
.cw-panel .cw-check-line { flex: 1 1 0; min-width: 0; justify-content: stretch; justify-items: start; }
.cw-panel .cw-check-line > .cw-check-text { justify-self: stretch; }
.cw-panel .cw-check-act { flex: none; }
.cw-panel .cw-check-item > .cw-fold { flex-basis: 100%; padding-left: 32px; }
.cw-panel .cw-check-item.is-open > .cw-fold { grid-template-rows: 1fr; }
.cw-panel .cw-check-item.is-open > .cw-fold .cw-why { padding: 0 8px 8px 0; }
.cw-panel .cw-list--flush { padding: 0; margin: 0 -6px; }

/* talking to an audience: the reader's avatar beside its words, the header names them */
.cw-panel .cw-heading.is-reader { display: flex; align-items: center; gap: 8px; }
.cw-panel .cw-heading.is-reader .cw-avatar { flex: none; }
.cw-panel .cw-heading-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.cw-panel .cw-heading.is-reader .cw-avatar, .cw-panel .cw-msg--reader .cw-msg-avatar { background: var(--accent-subtle); color: var(--accent-fg); }
.cw-panel .cw-msg--reader { display: grid; grid-template-columns: auto 1fr; column-gap: 8px; align-items: start; }
.cw-panel .cw-msg--reader > :not(.cw-msg-avatar) { grid-column: 2; }
.cw-panel .cw-msg--reader > .cw-msg-avatar { grid-column: 1; grid-row: 1 / span 9; margin-top: 1px; }
.cw-panel .cw-msg--reader .cw-msg-body { padding: 7px 11px; border-radius: 2px 10px 10px 10px; border: 1px solid var(--border-1); background: var(--surface); }
.cw-panel .cw-msg--reader > .cw-dots { grid-column: 2; padding-top: 6px; }
.cw-panel .cw-avatar--big { width: 44px; height: 44px; font-size: 15px; background: var(--accent-subtle); color: var(--accent-fg); }
.cw-panel .cw-empty-name { font-weight: 600; color: var(--fg-1); }
.cw-panel .cw-conversation > .cw-avatar { flex: none; width: 20px; height: 20px; font-size: 8.5px; }
.cw-panel .cw-talk-chip { padding-left: 3px; }
.cw-panel .cw-talk-chip .cw-avatar { width: 22px; height: 22px; background: var(--accent-subtle); color: var(--accent-fg); }
.cw-panel .cw-update { max-width: 100%; }
.cw-panel .cw-update[open] { flex-basis: 100%; }
.cw-panel .cw-update > summary { list-style: none; width: fit-content; }
.cw-panel .cw-update > summary::-webkit-details-marker { display: none; }
.cw-panel .cw-update[open] > summary { border-color: var(--accent); color: var(--accent-fg); }
.cw-panel .cw-diff { margin: 6px 0 0; padding: 8px 10px; border-left: 2px solid var(--accent); background: var(--surface-2); font-size: 12.5px; line-height: 1.5; animation: cw-in var(--base) var(--ease-out); }
.cw-panel .cw-diff dt { font: 500 11px/1.4 var(--mono); color: var(--fg-3); }
.cw-panel .cw-diff dt:not(:first-child) { margin-top: 6px; }
.cw-panel .cw-diff dd { margin: 0; color: var(--fg-2); }
/* word by word: only the words removed are struck, the words added marked */
.cw-panel .cw-diff del { color: var(--fg-4); text-decoration-thickness: 1px; }
.cw-panel .cw-diff ins { text-decoration: none; color: var(--fg-1); background: var(--accent-subtle); border-radius: 2px; box-decoration-break: clone; -webkit-box-decoration-break: clone; }
.cw-panel[data-layout="sheet"] .cw-diff { font-size: 14px; }

@media (prefers-reduced-motion: reduce) {
  .cw-panel *, .cw-panel *::before, .cw-panel *::after { animation-duration: 1ms !important; animation-iteration-count: 1 !important; transition-duration: 1ms !important; }
}
`;
