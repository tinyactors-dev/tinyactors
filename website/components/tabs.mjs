// <ta-tabs>: alternatives that say the same thing in different words (an
// install command per platform, a chart as a document or a diagram), one
// shown at a time. The page renders the tablist and panels, with the first
// panel shown; this makes the tabs work: a click or the arrow keys select a
// tab, and the choice holds for every <ta-tabs> on the page whose tablist
// has the same name (its aria-label), and for the next page (localStorage,
// when the browser keeps it). A link to an element inside a panel shows
// that panel. Showing a panel dispatches `ta-panel-shown`
// on it, so what a panel holds can load when it is first seen.

const KEY = 'tinyactors.tab.';

class Tabs extends HTMLElement {
  connectedCallback() {
    const tabs = this.tabs();
    if (tabs.length === 0 || this.dataset.ready) return;
    this.dataset.ready = 'true';
    tabs.forEach((tab, index) => {
      tab.addEventListener('click', () => choose(this.group(), tab.textContent));
      tab.addEventListener('keydown', (event) => {
        const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
        if (!step) return;
        const next = tabs[(index + step + tabs.length) % tabs.length];
        choose(this.group(), next.textContent);
        next.focus();
      });
    });
    const saved = read(this.group());
    if (saved) this.select(saved);
    // A link to something inside a panel (a release on the changelog page)
    // chooses that panel's tab, then shows the target.
    const target = location.hash ? document.getElementById(decodeURIComponent(location.hash.slice(1))) : null;
    const panel = target && this.contains(target) ? target.closest('[role="tabpanel"]') : null;
    const tab = panel ? tabs.find((candidate) => candidate.getAttribute('aria-controls') === panel.id) : null;
    if (tab) {
      choose(this.group(), tab.textContent);
      requestAnimationFrame(() => target.scrollIntoView());
    }
  }

  tabs() {
    return [...this.querySelectorAll('[role="tab"]')];
  }

  /** The name of the tablist: tabs of one name choose together. */
  group() {
    return this.querySelector('[role="tablist"]')?.getAttribute('aria-label') ?? '';
  }

  /** Shows the panel of the tab named `name`; nothing when there is none. */
  select(name) {
    const tabs = this.tabs();
    if (!tabs.some((tab) => tab.textContent === name)) return;
    for (const tab of tabs) {
      const selected = tab.textContent === name;
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
      const panel = this.querySelector(`#${CSS.escape(tab.getAttribute('aria-controls'))}`);
      panel.hidden = !selected;
      if (selected) panel.dispatchEvent(new CustomEvent('ta-panel-shown', { bubbles: true }));
    }
  }
}

function choose(group, name) {
  for (const tabs of document.querySelectorAll('ta-tabs')) if (tabs.group() === group) tabs.select(name);
  try {
    localStorage.setItem(KEY + group, name);
  } catch {
    // a private window, or storage turned off: the choice holds on this page
  }
}

function read(group) {
  try {
    return localStorage.getItem(KEY + group);
  } catch {
    return null;
  }
}

if (!customElements.get('ta-tabs')) customElements.define('ta-tabs', Tabs);
