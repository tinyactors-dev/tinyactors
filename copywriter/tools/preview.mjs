// The panel's development preview: mounts the panel over the mock reviewer.
// Query: ?layout=sidebar|sheet, ?theme=light|dark, ?key=0 (no API key),
// ?empty=1 (no annotations), ?delay=0.2 (scale the mock's waits),
// ?bare=1 (the panel alone, filling the viewport), ?shadow=0 (no Shadow DOM),
// ?read=0 (no reading of the page as an audience yet), ?suggest=1 (the page's
// audiences suggested, not yet accepted).
import { mountPanel } from '../src/ui/panel.mjs';
import { createMockReviewer } from '../src/ui/mock-reviewer.mjs';

const params = new URLSearchParams(location.search);
const layout = params.get('layout') === 'sheet' ? 'sheet' : 'sidebar';
const reviewer = createMockReviewer({
  hasKey: params.get('key') !== '0',
  empty: params.get('empty') === '1',
  delay: Number(params.get('delay') ?? 1),
  read: params.get('read') !== '0',
  suggest: params.get('suggest') === '1',
});
document.documentElement.dataset.layout = layout;
if (params.get('bare') === '1') document.documentElement.dataset.bare = '';
if (params.get('theme')) document.documentElement.dataset.theme = params.get('theme');

const host = document.getElementById('panel');
const root = params.get('shadow') === '0' ? host : host.attachShadow({ mode: 'open' });
const panel = mountPanel(root, reviewer, { layout });
if (params.get('theme')) (root.querySelector?.('.cw-panel') ?? host.querySelector('.cw-panel')).dataset.theme = params.get('theme');

// The sample page reports its selection to the mock, as a page adapter would.
const page = document.getElementById('page');
document.addEventListener('selectionchange', () => {
  const selection = document.getSelection();
  const text = selection && !selection.isCollapsed && page.contains(selection.anchorNode) ? selection.toString().trim() : '';
  reviewer.setSelection(text || null);
});
for (const button of document.querySelectorAll('[data-select]')) {
  button.addEventListener('click', () => reviewer.setSelection(button.dataset.select));
}

Object.assign(window, { reviewer, panel });
