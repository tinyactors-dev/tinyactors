// Scenario catalog: GET /api/scenarios (served by torture-bench), one
// tb-scenario-card per scenario. Custom elements import their <template>s.

import { shadow } from './sheet.js';

class Base extends HTMLElement {
  constructor() {
    super();
    shadow(this);
    this.refs = {};
    for (const el of this.shadowRoot.querySelectorAll('[data-ref]')) this.refs[el.dataset.ref] = el;
  }
}

class TbStatus extends Base {
  static observedAttributes = ['label'];
  attributeChangedCallback() {
    this.shadowRoot.querySelector('.pill-label').textContent = this.getAttribute('label') || '';
  }
  set(label, tone) {
    this.setAttribute('label', label);
    this.setAttribute('tone', tone);
    this.hidden = false;
  }
}

const minutes = (s) => (s >= 120 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`);

class TbScenarioCard extends Base {
  set scenario({ id, scenario: s, current, state }) {
    const title = s.title || s.name || id;
    const [head, ...rest] = title.split(' — ');
    this.refs.title.textContent = head;
    const sub = rest.join(' — ');
    this.refs.subtitle.textContent = sub.charAt(0).toUpperCase() + sub.slice(1);
    this.refs.subtitle.hidden = !rest.length;
    this.refs.summary.textContent = s.summary || (s.description || '').split(/(?<=\.)\s/)[0];
    this.refs.link.href = `chamber.html?scenario=${encodeURIComponent(id)}`;
    this.refs.link.setAttribute('aria-label', `Open ${head}`);
    const chips = [];
    if (s.duration) chips.push(minutes(s.duration));
    const systems = Object.keys(s.systems || {}).length;
    if (systems) chips.push(`${systems} system${systems > 1 ? 's' : ''}`);
    const mutations = Object.keys(s.mutations || {}).length;
    if (mutations) chips.push(`${mutations} mutation${mutations > 1 ? 's' : ''}`);
    if ((s.generators || []).length) chips.push('generated events');
    for (const io of s.ioprocessors || []) chips.push(`${io} I/O processor`);
    this.refs.chips.replaceChildren(
      ...chips.map((c) => {
        const li = document.createElement('li');
        li.textContent = c;
        return li;
      }),
    );
    if (current === id) this.refs.state.set(state === 'running' ? 'Running now' : 'Last run', state === 'running' ? 'accent' : 'neutral');
  }
}

class TbHome extends Base {
  constructor() {
    super();
    this.load();
  }
  async load() {
    try {
      const r = await fetch('/api/scenarios');
      const data = await r.json();
      const cards = data.scenarios.map((entry) => {
        const card = document.createElement('tb-scenario-card');
        card.scenario = { ...entry, current: data.current, state: data.state };
        return card;
      });
      this.refs.cards.replaceChildren(...cards);
      this.refs.status.set(data.current ? `${data.state === 'running' ? 'Running' : 'Last'}: ${data.current}` : 'Idle', data.state === 'running' ? 'accent' : 'neutral');
    } catch (e) {
      this.refs.error.hidden = false;
      this.refs.error.textContent = `Cannot load the scenarios (${e.message}). Is torture-bench running?`;
      this.refs.status.set('Offline', 'danger');
    }
  }
}

customElements.define('tb-status', TbStatus);
customElements.define('tb-scenario-card', TbScenarioCard);
customElements.define('tb-home', TbHome);
