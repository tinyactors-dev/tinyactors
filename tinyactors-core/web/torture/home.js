// Scenario catalog: GET /api/scenarios (served by torture-bench), one
// tb-scenario-card per scenario. Custom elements import their <template>s.
// Services that scenarios need (PostgreSQL) are shown live: the WebSocket
// pushes {type: "service"} messages; Start sends {cmd: "start-service"}.

import { serviceLabel, serviceTitle, serviceTone } from './service.js';
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
    this.needs = s.services || [];
    this.serviceChips = new Map();
    this.refs.chips.replaceChildren(
      ...this.needs.map((name) => {
        const li = document.createElement('li');
        li.className = 'needs';
        this.serviceChips.set(name, li);
        return li;
      }),
      ...chips.map((c) => {
        const li = document.createElement('li');
        li.textContent = c;
        return li;
      }),
    );
    for (const name of this.needs) this.service({ name, state: 'unknown' });
    if (current === id) this.refs.state.set(state === 'running' ? 'Running now' : 'Last run', state === 'running' ? 'accent' : 'neutral');
  }

  /** Live status of a service this scenario needs. */
  service(s) {
    const li = this.serviceChips?.get(s.name);
    if (!li) return;
    li.textContent = `Needs ${serviceTitle(s.name)} · ${serviceLabel(s).toLowerCase()}`;
    li.dataset.tone = serviceTone(s);
  }
}

class TbHome extends Base {
  constructor() {
    super();
    this.cards = [];
    this.addEventListener('start-service', (e) => this.send({ cmd: 'start-service', name: e.detail }));
    this.load();
  }
  send(msg) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(msg));
  }
  /** Live service status (only when a scenario needs a service). */
  connect(attempt = 0) {
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws`);
    this.ws = ws;
    ws.addEventListener('open', () => (this.refs.postgres.online = true));
    ws.addEventListener('message', (e) => {
      const m = JSON.parse(e.data);
      if (m.type === 'service') this.service(m);
    });
    ws.addEventListener('close', () => {
      this.refs.postgres.online = false;
      setTimeout(() => this.connect(attempt + 1), Math.min(5000, 500 * 2 ** attempt));
    });
  }
  service(s) {
    if (s.name === 'postgres') this.refs.postgres.update(s);
    for (const card of this.cards) card.service(s);
  }
  async load() {
    try {
      const r = await fetch('/api/scenarios');
      const data = await r.json();
      this.cards = data.scenarios.map((entry) => {
        const card = document.createElement('tb-scenario-card');
        card.scenario = { ...entry, current: data.current, state: data.state };
        return card;
      });
      this.refs.cards.replaceChildren(...this.cards);
      const needed = new Set(data.scenarios.flatMap((e) => e.scenario.services || []));
      if (needed.has('postgres')) {
        this.refs.services.hidden = false;
        this.service(data.services.postgres);
        this.connect();
      }
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
