// tb-service: live status of an external service a scenario needs (today:
// the PostgreSQL instance of bindings/postgres). torture-bench probes it every
// second while a dashboard is connected and pushes {type: "service"} messages
// over the WebSocket; the Start button sends {cmd: "start-service"}, which runs
// `mise run pg:start` (idempotent: initdb, start, CREATE DATABASE/EXTENSION).
// Used by both pages; each page supplies the <template id="tb-service">.

import { shadow } from './sheet.js';

const STATES = {
  unknown: ['Checking', 'neutral'],
  available: ['Available', 'ok'],
  starting: ['Starting', 'warn'],
  missing: ['Not set up', 'warn'],
  unavailable: ['Not running', 'danger'],
  failed: ['Start failed', 'danger'],
};
const TITLES = { postgres: 'PostgreSQL' };

export const serviceTitle = (name) => TITLES[name] || name;

/** A one-line status for compact places (scenario cards). */
export function serviceLabel(s) {
  return (STATES[s.state] || STATES.unknown)[0];
}
export function serviceTone(s) {
  return (STATES[s.state] || STATES.unknown)[1];
}

export class TbService extends HTMLElement {
  constructor() {
    super();
    shadow(this, 'tb-service');
    this.refs = {};
    for (const el of this.shadowRoot.querySelectorAll('[data-ref]')) this.refs[el.dataset.ref] = el;
    this.refs.start.addEventListener('click', () => {
      this.refs.start.disabled = true;
      this.dispatchEvent(new CustomEvent('start-service', { bubbles: true, composed: true, detail: this.name }));
    });
  }
  /** s: { name, state, detail, version, extension, address, start } */
  update(s) {
    this.name = s.name;
    const [label, tone] = STATES[s.state] || STATES.unknown;
    this.refs.title.textContent = serviceTitle(s.name);
    this.refs.state.set(label, tone);
    this.refs.detail.textContent =
      s.detail || (s.state === 'available' ? 'Ready. Scenarios that need it can run.' : '');
    this.refs.detail.hidden = !this.refs.detail.textContent;
    this.refs.address.textContent = s.address || '–';
    this.refs.version.textContent = s.version || '–';
    this.refs.extension.textContent = s.extension ? `tinyactors ${s.extension}` : '–';
    const startable = !['available', 'starting', 'unknown'].includes(s.state);
    this.refs.start.hidden = !startable;
    this.refs.start.disabled = !startable || !this.connected;
    this.refs.start.textContent = s.state === 'failed' ? `Try again` : `Start ${serviceTitle(s.name)}`;
    this.refs.command.textContent = s.start || '';
    this.refs.how.hidden = !startable || !s.start;
    this.refs.actions.hidden = !startable; // no empty row under the facts
    this.setAttribute('state', s.state);
  }
  /** Disable the button while the WebSocket is down. */
  set online(v) {
    this.connected = v;
    if (this.name) this.refs.start.disabled = !v || this.refs.start.hidden;
  }
}

customElements.define('tb-service', TbService);
