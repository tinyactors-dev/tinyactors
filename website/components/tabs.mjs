// <ta-tabs>: alternatives that say the same thing in different words (an
// install command per platform, a chart as a document or a diagram), one
// shown at a time. The page renders the tablist and panels, with the first
// panel shown; this makes the tabs work: a click or the arrow keys select a
// tab, and the choice holds for every <ta-tabs> on the page whose tablist
// has the same name (its aria-label), and for the next page (localStorage,
// when the browser keeps it). A tablist's data-group names its group
// instead (a program's packages choose with the install commands), and a
// <ta-tabs data-local> chooses for itself only, remembering nothing (a
// program's files). Tabs may nest: each <ta-tabs> works its own tablist,
// not those of the <ta-tabs> inside its panels. A link to an element
// inside a panel shows that panel. Showing a panel dispatches `ta-panel-shown`
// on it, so what a panel holds can load when it is first seen.

const KEY = 'tinyactors.tab.';

class Tabs extends HTMLElement {
  connectedCallback() {
    const tabs = this.tabs();
    if (tabs.length === 0 || this.dataset.ready) return;
    this.dataset.ready = 'true';
    tabs.forEach((tab, index) => {
      tab.addEventListener('click', () => this.choose(tab.textContent));
      tab.addEventListener('keydown', (event) => {
        const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
        if (!step) return;
        const next = tabs[(index + step + tabs.length) % tabs.length];
        this.choose(next.textContent);
        next.focus();
      });
    });
    const saved = this.local() ? null : read(this.group());
    if (saved) this.select(saved);
    this.reveal();
    addEventListener('hashchange', () => this.reveal());
  }

  /** A link to something inside a panel (a release on the changelog page) chooses that panel's tab, then shows the target. */
  reveal() {
    const target = location.hash ? document.getElementById(decodeURIComponent(location.hash.slice(1))) : null;
    if (!target || !this.contains(target)) return;
    const panels = new Map(this.tabs().map((tab) => [tab.getAttribute('aria-controls'), tab]));
    let panel = target.closest('[role="tabpanel"]');
    while (panel && !panels.has(panel.id)) panel = panel.parentElement?.closest('[role="tabpanel"]');
    const tab = panel && panels.get(panel.id);
    if (!tab) return;
    this.choose(tab.textContent);
    requestAnimationFrame(() => target.scrollIntoView());
  }

  /** This element's own tabs, not those of a <ta-tabs> inside it. */
  tabs() {
    return [...this.querySelectorAll('[role="tab"]')].filter((tab) => tab.closest('ta-tabs') === this);
  }

  /** The name of the tablist: tabs of one name choose together. */
  group() {
    const list = [...this.querySelectorAll('[role="tablist"]')].find((candidate) => candidate.closest('ta-tabs') === this);
    return list?.dataset.group ?? list?.getAttribute('aria-label') ?? '';
  }

  local() {
    return this.hasAttribute('data-local');
  }

  /** Chooses the tab named `name` here, and in every <ta-tabs> of the group unless this one is local. */
  choose(name) {
    if (this.local()) this.select(name);
    else choose(this.group(), name);
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
  for (const tabs of document.querySelectorAll('ta-tabs')) if (!tabs.local() && tabs.group() === group) tabs.select(name);
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
