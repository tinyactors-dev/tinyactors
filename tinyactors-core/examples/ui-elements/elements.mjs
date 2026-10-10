/**
 * Custom elements backed by actors. Every element owns an actor (a chart
 * from charts.mjs) in one tinyactors system that runs in the page
 * (WebAssembly); the element forwards DOM events as actor events and renders
 * the actor's snapshot — nothing else. Each element clones its <template>.
 *
 * Off-screen counters are hibernated (unloaded from the wasm memory into a
 * store) and wake up by themselves when they are used again.
 */
import * as ta from '../../bindings/web/index.mjs';
import { uiCharts } from './charts.mjs';
import { TaEditor } from './editor-element.mjs';

const { createSystem, createActor } = ta;
export const system = createSystem({ systemID: 'ui', hibernation: { store: new Map() } });
const charts = uiCharts(ta);

/** Clones <template id=localName>, owns an actor, renders its snapshots. */
class ActorElement extends HTMLElement {
  static chart = null;
  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).append(document.importNode(document.getElementById(this.localName).content, true));
    this.$ = (sel) => this.shadowRoot.querySelector(sel);
  }
  input() {
    return undefined;
  }
  connectedCallback() {
    if (this.actor) return;
    this.actor = createActor(charts[this.constructor.chart], { system, input: this.input() }).start();
    this.wire();
    const chip = this.$('.state');
    if (chip) chip.addEventListener('click', () => showSCXML(this.actor.chart));
    this.toggleAttribute('debug', document.body.classList.contains('show-states'));
    const draw = (s) => {
      if (chip) chip.textContent = JSON.stringify(s.value).replace(/"/g, '') + (this.actor.hibernated ? ' · hibernated' : '');
      this.render(s);
    };
    draw(this.actor.getSnapshot());
    this.actor.subscribe(draw);
  }
  disconnectedCallback() {
    this.actor?.stop();
    this.actor = null;
  }
  send(type, extra) {
    this.actor.send({ type, ...extra });
  }
}

class TaToggle extends ActorElement {
  static chart = 'toggle';
  static observedAttributes = ['disabled'];
  input() {
    return { disabled: this.hasAttribute('disabled') };
  }
  attributeChangedCallback() {
    if (this.actor) this.send(this.hasAttribute('disabled') ? 'DISABLE' : 'ENABLE');
  }
  wire() {
    this.$('button').addEventListener('click', () => this.send('TOGGLE'));
  }
  render(s) {
    const b = this.$('button');
    b.setAttribute('aria-checked', String(s.value === 'on'));
    b.disabled = s.context.disabled;
    this.$('.label').textContent = this.getAttribute('label') || '';
    if (s.value !== this.last) this.dispatchEvent(new CustomEvent('change', { detail: { on: s.value === 'on' } }));
    this.last = s.value;
  }
}

class TaCounter extends ActorElement {
  static chart = 'counter';
  input() {
    const num = (k, d) => (this.hasAttribute(k) ? Number(this.getAttribute(k)) : d);
    return { value: num('value', 0), min: num('min', -Infinity), max: num('max', Infinity), step: num('step', 1) };
  }
  wire() {
    for (const [sel, dir] of [['.dec', -1], ['.inc', 1]]) {
      const b = this.$(sel);
      b.addEventListener('pointerdown', (e) => {
        b.setPointerCapture(e.pointerId);
        this.send('PRESS', { dir });
      });
      for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) b.addEventListener(ev, () => this.send('RELEASE'));
    }
    this.$('.value').addEventListener('keydown', (e) => {
      if (e.key === 'ArrowUp' || e.key === 'ArrowRight') this.send('INC');
      if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') this.send('DEC');
    });
  }
  render(s) {
    const c = s.context;
    this.$('.value').textContent = c.value;
    this.$('.dec').disabled = c.value <= c.min;
    this.$('.inc').disabled = c.value >= c.max;
  }
}

class TaSearch extends ActorElement {
  static chart = 'search';
  input() {
    return { words: (this.getAttribute('words') || '').split(',').map((w) => w.trim()).filter(Boolean) };
  }
  wire() {
    const input = this.$('input');
    input.addEventListener('input', () => this.send('TYPE', { value: input.value }));
    input.addEventListener('keydown', (e) => {
      const key = { ArrowDown: 'DOWN', ArrowUp: 'UP', Enter: 'PICK', Escape: 'ESCAPE' }[e.key];
      if (key) {
        e.preventDefault();
        this.send(key);
      }
    });
    this.$('ul').addEventListener('mousedown', (e) => {
      const li = e.target.closest('li');
      if (li) this.send('PICK', { index: Number(li.dataset.index) });
    });
  }
  render(s) {
    const c = s.context;
    const input = this.$('input');
    if (input.value !== c.query && document.activeElement !== this) input.value = c.query;
    if (s.value === 'idle' && c.picked && input.value !== c.query) input.value = c.query;
    this.$('.spinner').hidden = s.value !== 'searching';
    const ul = this.$('ul');
    ul.hidden = s.value !== 'open';
    ul.replaceChildren(
      ...(c.results.length ? c.results : ['no matches']).map((w, i) => {
        const li = document.createElement('li');
        li.textContent = w;
        if (c.results.length) li.dataset.index = i;
        else li.className = 'empty';
        if (i === c.active) li.setAttribute('aria-selected', 'true');
        return li;
      }),
    );
    this.$('.picked').textContent = c.picked ? `picked: ${c.picked}` : '';
  }
}

class TaForm extends ActorElement {
  static chart = 'form';
  input() {
    return { names: [...this.querySelectorAll('ta-field')].map((f) => f.getAttribute('name')) };
  }
  wire() {
    this.$('form').addEventListener('submit', (e) => {
      e.preventDefault();
      this.send('SUBMIT');
    });
    this.$('.again').addEventListener('click', () => this.send('RESET'));
  }
  render(s) {
    const valid = Object.values(s.context.fields).every((f) => f.valid);
    const submit = this.$('.submit');
    submit.disabled = !valid || s.value !== 'editing';
    submit.textContent = s.value === 'submitting' ? 'Sending…' : 'Sign up';
    this.$('.done').hidden = s.value !== 'done';
    this.$('.done-text').textContent = s.context.sent ? `Welcome, ${s.context.sent.name}!` : '';
  }
}

class TaField extends ActorElement {
  static chart = 'field';
  input() {
    /* the field reports to its form's actor (actor-to-actor, inside the page) */
    const form = this.closest('ta-form');
    if (form && !form.actor) form.connectedCallback();
    return { name: this.getAttribute('name'), rule: this.getAttribute('rule'), form: form?.actor };
  }
  wire() {
    this.$('.label').textContent = this.getAttribute('label') || this.getAttribute('name');
    const input = this.$('input');
    input.type = this.getAttribute('type') || 'text';
    input.addEventListener('input', () => this.send('INPUT', { value: input.value }));
    input.addEventListener('blur', () => this.send('BLUR'));
  }
  render(s) {
    const show = s.value === 'checked' && s.context.error;
    this.$('.error').textContent = show ? s.context.error : '';
    this.$('input').toggleAttribute('aria-invalid', !!show);
    this.$('.ok').hidden = !!s.context.error || s.value === 'pristine';
  }
}

class TaDialog extends ActorElement {
  static chart = 'dialog';
  wire() {
    this.$('.open').addEventListener('click', () => this.send('OPEN'));
    this.$('.close').addEventListener('click', () => this.send('CLOSE'));
    this.$('.backdrop').addEventListener('click', (e) => e.target === e.currentTarget && this.send('CLOSE'));
    document.addEventListener('keydown', (e) => e.key === 'Escape' && this.actor?.send({ type: 'ESCAPE' }));
  }
  render(s) {
    this.$('.backdrop').dataset.phase = s.value;
  }
}

for (const [name, cls] of Object.entries({ 'ta-editor': TaEditor, 'ta-toggle': TaToggle, 'ta-counter': TaCounter, 'ta-search': TaSearch, 'ta-form': TaForm, 'ta-field': TaField, 'ta-dialog': TaDialog }))
  customElements.define(name, cls);

/* ---- "show states": every element shows its state; click one for its SCXML */
function showSCXML(chart) {
  const d = document.getElementById('scxml');
  d.querySelector('h2').textContent = `${chart.name} — the SCXML this element runs`;
  d.querySelector('pre').textContent = chart.xml;
  d.showModal();
}
document.addEventListener('inspect-chart', (event) => showSCXML(event.detail));
document.getElementById('show-states').addEventListener('change', (e) => {
  document.body.classList.toggle('show-states', e.target.checked);
  for (const el of document.querySelectorAll('*')) if (el instanceof ActorElement || el instanceof TaEditor) el.toggleAttribute('debug', e.target.checked);
});

/* ---- hibernation: counters scrolled out of view are unloaded */
const many = document.getElementById('many');
for (let i = 1; i <= 200; i++) {
  const c = document.createElement('ta-counter');
  c.setAttribute('value', String(i));
  c.setAttribute('min', '0');
  c.setAttribute('max', '999');
  many.append(c);
}
const seen = new IntersectionObserver(
  (entries) => {
    for (const e of entries) {
      if (e.isIntersecting || !e.target.actor) continue;
      try {
        system.hibernate(e.target.actor);
      } catch {
        /* busy (a held button repeating): it stays */
      }
    }
  },
  { root: many },
);
for (const c of many.children) seen.observe(c);
setInterval(() => {
  const st = system.stats();
  document.getElementById('stats').textContent = `${st.actors} actors · ${st.hibernated} hibernated`;
}, 300);
