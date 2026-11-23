// The extension's side panel: the reviewer runs here, in the panel's own
// page, with adapters for the browser (AGENTS.md, "Platform adapters"):
//
// - page: a proxy to the content script of the tab under review, which it
//   injects with chrome.scripting when it is missing. The panel follows its
//   window's active tab, and a navigation or reload of that tab
//   (page.onNavigate). Opened with ?tab=<id> (the tests open it as a tab of
//   its own), it reviews that tab only.
// - credentials: chrome.storage.session, or chrome.storage.local when
//   remembered; neither is readable by content scripts.
// - download: a blob link; clipboard: navigator.clipboard.
// - recorder: the core's, after the microphone was allowed once in a tab
//   (mic.html): a side panel cannot show Chrome's permission prompt.

import { createReviewer } from '../../core/reviewer.mjs';
import { createRecorder } from '../../core/recorder.mjs';
import { mountPanel } from '../../ui/panel.mjs';

const KEY = 'geminiKey';
const params = new URLSearchParams(location.search);
const fixedTab = params.has('tab') ? Number(params.get('tab')) : null;

// ---- the page under review ------------------------------------------------

function createTabPage() {
  let tabID = fixedTab;
  const selectionListeners = new Set();
  const navigateListeners = new Set();
  const pickEndListeners = new Set();
  let pickingTab = null; // the tab in pick mode: told to stop when the panel leaves it
  let ready = null; // a promise of the tab's id, once found

  async function activeTab() {
    if (fixedTab !== null) return fixedTab;
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    return tab?.id ?? null;
  }

  // The content script answers, or is injected and then answers; null when
  // the page cannot take one (chrome://, the web store, a PDF viewer).
  async function call(method, ...args) {
    tabID = await (ready ??= activeTab());
    if (tabID === null) return null;
    const message = { cw: 'page', method, args };
    let answer;
    try {
      answer = await chrome.tabs.sendMessage(tabID, message);
    } catch {
      try {
        await chrome.scripting.executeScript({ target: { tabId: tabID }, files: ['content.js'] });
        answer = await chrome.tabs.sendMessage(tabID, message);
      } catch {
        return null;
      }
    }
    if (answer?.error) throw new Error(answer.error);
    return answer?.value ?? null;
  }

  chrome.runtime.onMessage.addListener((message, sender) => {
    if (sender.tab?.id !== tabID) return;
    if (message?.cw === 'selection') for (const fn of selectionListeners) fn(message.selection);
    if (message?.cw === 'pick' && !message.on) {
      pickingTab = null;
      for (const fn of pickEndListeners) fn();
    }
  });

  const navigate = () => {
    if (pickingTab !== null && pickingTab !== tabID) {
      chrome.tabs.sendMessage(pickingTab, { cw: 'page', method: 'pick', args: [false] }).catch(() => { /* gone */ });
      pickingTab = null;
    }
    for (const fn of navigateListeners) fn();
  };
  if (fixedTab === null) {
    chrome.windows.getCurrent().then((win) => {
      chrome.tabs.onActivated.addListener((active) => {
        const id = active.tabId;
        if (active.windowId !== win.id || id === tabID) return;
        tabID = id;
        ready = Promise.resolve(id);
        navigate();
      });
    });
  }
  chrome.tabs.onUpdated.addListener((id, change) => {
    if (id === tabID && change.status === 'complete') navigate();
  });

  return {
    async info() {
      const info = await call('info');
      if (info) return info;
      const tab = tabID === null ? null : await chrome.tabs.get(tabID).catch(() => null);
      return { url: tab?.url || '', title: tab?.title || '', lang: 'en' };
    },
    context: async () => (await call('context')) || { headings: [], text: '' },
    purpose: () => call('purpose'),
    selection: () => call('selection'),
    onSelection: (fn) => selectionListeners.add(fn),
    locate: async (anchor) => Boolean(await call('locate', anchor)),
    anchor: (text) => call('anchor', text),
    preview: async (request) => Boolean(await call('preview', request)),
    revert: async (request) => Boolean(await call('revert', request)),
    highlight: (anchor) => call('highlight', anchor),
    async pick(on) {
      const ok = Boolean(await call('pick', on));
      pickingTab = on && ok ? tabID : null;
      return ok;
    },
    onPickEnd: (fn) => pickEndListeners.add(fn),
    onNavigate: (fn) => navigateListeners.add(fn),
  };
}

// ---- the key, the export, the microphone ------------------------------------

const credentials = {
  async get() {
    const session = await chrome.storage.session.get(KEY);
    if (session[KEY]) return session[KEY];
    const local = await chrome.storage.local.get(KEY);
    return local[KEY] || null;
  },
  async set(key, { remember = false } = {}) {
    if (remember) {
      await chrome.storage.local.set({ [KEY]: key });
      await chrome.storage.session.remove(KEY);
    } else {
      await chrome.storage.session.set({ [KEY]: key });
      await chrome.storage.local.remove(KEY);
    }
  },
  async forget() {
    await chrome.storage.session.remove(KEY);
    await chrome.storage.local.remove(KEY);
  },
  async remembered() {
    return Boolean((await chrome.storage.local.get(KEY))[KEY]);
  },
};

function download(filename, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/zip' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

const clipboard = (text) => navigator.clipboard.writeText(text);

// Chrome shows no permission prompt in a side panel: getUserMedia there is
// refused until the extension's origin was granted the microphone, which
// mic.html asks for in a tab of its own, once.
function createPanelRecorder() {
  const recorder = createRecorder();
  return {
    get recording() { return recorder.recording; },
    async start() {
      let state = 'prompt';
      try { state = (await navigator.permissions.query({ name: 'microphone' })).state; } catch { /* unknown */ }
      if (state !== 'granted') {
        await chrome.tabs.create({ url: chrome.runtime.getURL('mic.html') });
        throw new Error('Allow the microphone in the tab that opened, then hold to talk again.');
      }
      return recorder.start();
    },
    stop: () => recorder.stop(),
    cancel: () => recorder.cancel(),
  };
}

// ---- the panel ----------------------------------------------------------------

const reviewer = createReviewer({
  page: createTabPage(),
  credentials,
  download,
  clipboard,
  recorder: createPanelRecorder(),
  wasmURL: chrome.runtime.getURL('tinyactors.wasm'),
});
const panel = mountPanel(document.body, reviewer, { layout: 'sidebar' });
addEventListener('pagehide', () => { panel.unmount?.(); reviewer.close(); });
Object.assign(globalThis, { reviewer });
