// <ta-tabs>: alternatives that say the same thing in different words (an
// install command per platform), one shown at a time. The page renders the
// tablist and panels, with the first panel shown; this makes the tabs work:
// a click or the arrow keys select a tab, and the choice holds for every
// <ta-tabs> on the page with a tab of the same name, and for the next page
// (localStorage, when the browser keeps it).

const KEY = 'tinyactors.tab';

class Tabs extends HTMLElement {
  connectedCallback() {
    const tabs = this.tabs();
    if (tabs.length === 0 || this.dataset.ready) return;
    this.dataset.ready = 'true';
    tabs.forEach((tab, index) => {
      tab.addEventListener('click', () => choose(tab.textContent));
      tab.addEventListener('keydown', (event) => {
        const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
        if (!step) return;
        const next = tabs[(index + step + tabs.length) % tabs.length];
        choose(next.textContent);
        next.focus();
      });
    });
    const saved = read();
    if (saved) this.select(saved);
  }

  tabs() {
    return [...this.querySelectorAll('[role="tab"]')];
  }

  /** Shows the panel of the tab named `name`; nothing when there is none. */
  select(name) {
    const tabs = this.tabs();
    if (!tabs.some((tab) => tab.textContent === name)) return;
    for (const tab of tabs) {
      const selected = tab.textContent === name;
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
      this.querySelector(`#${CSS.escape(tab.getAttribute('aria-controls'))}`).hidden = !selected;
    }
  }
}

function choose(name) {
  for (const tabs of document.querySelectorAll('ta-tabs')) tabs.select(name);
  try {
    localStorage.setItem(KEY, name);
  } catch {
    // a private window, or storage turned off: the choice holds on this page
  }
}

function read() {
  try {
    return localStorage.getItem(KEY);
  } catch {
    return null;
  }
}

customElements.define('ta-tabs', Tabs);
