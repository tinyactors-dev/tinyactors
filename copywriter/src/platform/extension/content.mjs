// The extension's content script: the page adapter's DOM side (../dom.mjs)
// in the reviewed tab, answering the side panel's calls and telling it when
// the selection changes. Bundled as a classic script (content.js): the
// manifest injects it into every http(s) page, and the side panel injects
// it again with chrome.scripting where it is missing (a tab opened before
// the extension was loaded).
//
// Messages, both ways through chrome.runtime:
//   panel → page  {cw: 'page', method, args} → {value} | {error}
//   page → panel  {cw: 'selection', selection: {text, anchor} | null}
//                 {cw: 'pick', on: false}   (Escape ended pick mode)
//
// Injected twice into one document, the second copy finds the first and
// stops; a copy left by an extension that was reloaded (its runtime gone)
// is disposed of, its previews put back, and replaced.

import { createDOMPage } from '../dom.mjs';

const KEY = Symbol.for('tinyactors-copywriter');

function alive(runtime) {
  try { return Boolean(runtime?.id); } catch { return false; }
}

(() => {
  const prior = globalThis[KEY];
  if (prior && alive(prior.runtime)) return;
  try { prior?.dispose(); } catch { /* gone with its context */ }

  const page = createDOMPage();
  const methods = {
    ping: () => true,
    info: () => page.info(),
    context: () => page.context(),
    purpose: () => page.purpose(),
    selection: () => page.selection(),
    locate: (anchor) => page.locate(anchor),
    anchor: (text) => page.anchor(text),
    preview: (request) => page.preview(request),
    revert: (request) => page.revert(request),
    highlight: (anchor) => page.highlight(anchor),
    pick: (on) => page.pick(on),
  };

  function listener(message, _sender, sendResponse) {
    if (!message || message.cw !== 'page') return undefined;
    const fn = methods[message.method];
    if (!fn) {
      sendResponse({ error: `copywriter: no page method ${message.method}` });
      return undefined;
    }
    try {
      sendResponse({ value: fn(...(message.args || [])) ?? null });
    } catch (e) {
      sendResponse({ error: e?.message || String(e) });
    }
    return undefined;
  }
  chrome.runtime.onMessage.addListener(listener);

  page.onSelection((selection) => {
    if (!alive(chrome.runtime)) return;
    chrome.runtime.sendMessage({ cw: 'selection', selection }).catch(() => { /* no panel open */ });
  });

  page.onPickEnd(() => {
    if (!alive(chrome.runtime)) return;
    chrome.runtime.sendMessage({ cw: 'pick', on: false }).catch(() => { /* no panel open */ });
  });

  globalThis[KEY] = {
    runtime: chrome.runtime,
    dispose() {
      page.dispose();
      try { chrome.runtime.onMessage.removeListener(listener); } catch { /* invalidated */ }
    },
  };
})();
