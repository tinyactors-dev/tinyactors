// The extension's service worker: it only makes the toolbar button open
// the side panel. Everything else runs in the side panel page
// (sidepanel.mjs), so the worker may stop whenever Chrome likes.

const openOnClick = () => chrome.sidePanel.setPanelBehavior({ openPanelOnActionClick: true }).catch(() => {});

chrome.runtime.onInstalled.addListener(openOnClick);
chrome.runtime.onStartup.addListener(openOnClick);
