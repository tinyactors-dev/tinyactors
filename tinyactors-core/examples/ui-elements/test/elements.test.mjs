// The custom elements themselves (elements.mjs, as the page loads it with the
// browser build of the web binding), on a minimal DOM: every element is
// defined with customElements.define, owns an actor while connected, turns
// DOM events into actor events and renders the actor's snapshots.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

/* ---- a minimal DOM: nodes are created on first lookup (querySelector,
 * getElementById), so the test checks wiring, not the page's templates */
const registry = new Map(); /* name -> class */
const nameOf = new Map(); /* class -> name */
class Node {
  constructor(localName = 'div') {
    this._name = localName;
    this.children = [];
    this.parent = null;
    this.attrs = new Map();
    this.listeners = {};
    this.lookups = new Map();
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.disabled = false;
    this.textContent = '';
    this.value = '';
    this.content = this;
    const cls = new Set();
    this.classList = { contains: (c) => cls.has(c), add: (c) => cls.add(c), toggle: (c, on = !cls.has(c)) => (on ? cls.add(c) : cls.delete(c), on) };
  }
  get localName() {
    return nameOf.get(this.constructor) || this._name;
  }
  getAttribute(k) {
    return this.attrs.has(k) ? this.attrs.get(k) : null;
  }
  setAttribute(k, v) {
    this.attrs.set(k, String(v));
  }
  hasAttribute(k) {
    return this.attrs.has(k);
  }
  removeAttribute(k) {
    this.attrs.delete(k);
  }
  toggleAttribute(k, on = !this.attrs.has(k)) {
    on ? this.attrs.set(k, '') : this.attrs.delete(k);
    return on;
  }
  addEventListener(type, fn) {
    (this.listeners[type] ||= []).push(fn);
  }
  dispatchEvent(ev) {
    for (const fn of this.listeners[ev.type] || []) fn(ev);
    return true;
  }
  fire(type, extra = {}) {
    return this.dispatchEvent({ type, target: this, preventDefault() {}, ...extra });
  }
  querySelector(sel) {
    if (!this.lookups.has(sel)) this.lookups.set(sel, new Node(sel.replace(/^\./, '')));
    return this.lookups.get(sel);
  }
  querySelectorAll(tag) {
    const out = [];
    const walk = (n) => n.children.forEach((k) => (k.localName === tag && out.push(k), walk(k)));
    walk(this);
    return out;
  }
  closest(tag) {
    for (let n = this.parent; n; n = n.parent) if (n.localName === tag) return n;
    return null;
  }
  append(...nodes) {
    for (const n of nodes) {
      n.parent = this;
      this.children.push(n);
    }
  }
  replaceChildren(...nodes) {
    this.children = [];
    this.append(...nodes);
  }
  attachShadow() {
    return (this.shadowRoot = new Node('#shadow'));
  }
  setPointerCapture() {}
  setSelectionRange(start, end, direction) {
    this.selectionStart = start;
    this.selectionEnd = end;
    this.selectionDirection = direction;
  }
}
const ids = new Map();
globalThis.HTMLElement = Node;
globalThis.customElements = {
  define(name, cls) {
    registry.set(name, cls);
    nameOf.set(cls, name);
  },
  get: (name) => registry.get(name),
};
globalThis.document = {
  body: new Node('body'),
  activeElement: null,
  getElementById: (id) => ids.get(id) || (ids.set(id, new Node(id)), ids.get(id)),
  importNode: (n) => n,
  createElement: (name) => (registry.has(name) ? new (registry.get(name))() : new Node(name)),
  addEventListener() {},
  querySelectorAll: () => [],
};
globalThis.IntersectionObserver = class {
  observe() {}
};
/* the browser build loads its wasm with fetch(); setImmediate stands in for MessageChannel turns */
const realFetch = globalThis.fetch;
globalThis.fetch = async (url) =>
  String(url).startsWith('file:') ? new Response(readFileSync(fileURLToPath(url)), { headers: { 'content-type': 'application/wasm' } }) : realFetch(url);
globalThis.MessageChannel = class {
  constructor() {
    const port1 = { onmessage: null };
    this.port1 = port1;
    this.port2 = { postMessage: (data) => setImmediate(() => port1.onmessage?.({ data })) };
  }
};
const realSetInterval = globalThis.setInterval;
const intervals = new Map();
globalThis.setInterval = (fn, ms) => {
  const timer = realSetInterval(fn, ms).unref();
  intervals.set(timer, fn);
  return timer;
};

const { system } = await import('../elements.mjs');
test.after(() => {
  for (const timer of intervals.keys()) clearInterval(timer);
  globalThis.setInterval = realSetInterval;
  system.stop();
});
const until = async (cond, what, limit = 2000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > limit) throw new Error(`timed out: ${what}`);
    await new Promise((r) => setTimeout(r, 2));
  }
};
const mount = (name, attrs = {}) => {
  const el = document.createElement(name);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  el.connectedCallback();
  return el;
};

test('elements: every custom element is defined and owns a running actor while connected', () => {
  assert.deepEqual([...registry.keys()].sort(), ['ta-counter', 'ta-dialog', 'ta-editor', 'ta-field', 'ta-form', 'ta-search', 'ta-toggle']);
  for (const name of registry.keys()) {
    const el = mount(name, { name: 'f', words: 'a,b' });
    assert.equal(el.actor.getSnapshot().status, 'active', name);
    assert.equal(el.actor.logic.id, name.slice(3), `${name} runs the ${name.slice(3)} machine`);
    const actor = el.actor;
    el.disconnectedCallback();
    assert.equal(el.actor, null);
    assert.equal(actor.getSnapshot().status, 'stopped', `${name}: the actor stops with the element`);
  }
});

test('elements: DOM events drive the actor, snapshots render', async () => {
  const toggle = mount('ta-toggle', { label: 'Wi-Fi' });
  const button = toggle.shadowRoot.querySelector('button');
  assert.equal(button.getAttribute('aria-checked'), 'false');
  button.fire('click');
  await until(() => toggle.actor.getSnapshot().value === 'on', 'TOGGLE');
  assert.equal(button.getAttribute('aria-checked'), 'true');
  assert.equal(toggle.shadowRoot.querySelector('.label').textContent, 'Wi-Fi');
  toggle.setAttribute('disabled', '');
  toggle.attributeChangedCallback();
  await until(() => button.disabled, 'DISABLE renders');

  const counter = mount('ta-counter', { value: '5', max: '6' });
  const value = counter.shadowRoot.querySelector('.value');
  assert.equal(value.textContent, 5);
  counter.shadowRoot.querySelector('.inc').fire('pointerdown', { pointerId: 1 });
  counter.shadowRoot.querySelector('.inc').fire('pointerup');
  await until(() => value.textContent === 6, 'PRESS renders the new value');
  assert.equal(counter.shadowRoot.querySelector('.inc').disabled, true, 'at the maximum');
  value.fire('keydown', { key: 'ArrowDown' });
  await until(() => value.textContent === 5, 'DEC');

  const dialog = mount('ta-dialog');
  dialog.shadowRoot.querySelector('.open').fire('click');
  await until(() => dialog.actor.getSnapshot().value !== 'closed', 'OPEN');
  assert.equal(dialog.shadowRoot.querySelector('.backdrop').dataset.phase, dialog.actor.getSnapshot().value);
  for (const el of [toggle, counter, dialog]) el.disconnectedCallback();
});

test('editor element: keyboard, focus, paste, composition, selection, migration and reconnect', async (t) => {
  const el = mount('ta-editor');
  t.after(() => el.disconnectedCallback());
  const area = el.$('textarea');
  const keys = async (...keys) => {
    for (const key of keys) { area.fire('keydown', { key }); el.system.flush(); }
    await until(() => !el.actor.getSnapshot().error, 'keyboard');
  };
  area.fire('focus');
  await keys('3', 'd', 'w');
  assert.ok(area.value.startsWith('four five'));
  assert.equal(el.$('.focus-state').textContent, 'focused');
  await keys('u', 'i', 'j');
  assert.equal(el.$('.sequence-state').textContent, 'j');
  assert.equal(el.$('.light-insert').dataset.active, 'true');
  area.fire('paste', { clipboardData: { getData: () => 'pasted' } }); el.system.flush();
  assert.ok(area.value.startsWith('jpastedone'));
  area.fire('compositionstart'); el.system.flush();
  await keys('x'); // ignored while composing
  area.fire('compositionend', { data: '日本' }); el.system.flush();
  assert.ok(area.value.startsWith('jpasted日本one'));
  await keys('Escape');
  const address = el.actor.address, text = area.value;
  el.$('.keymap').value = 'emacs'; el.$('.keymap').fire('change');
  await until(() => !el.upgrading, 'keymap migration');
  assert.equal(area.value, text); assert.equal(el.actor.address, address);
  assert.equal(el.$('.mode-name').textContent, 'EMACS');
  area.setSelectionRange(1, 4, 'backward'); area.fire('pointerup'); el.system.flush();
  assert.equal(el.actor.getSnapshot().context.cursor, 1);
  assert.equal(el.actor.getSnapshot().context.mark, 4);
  area.fire('blur'); el.system.flush(); assert.equal(el.$('.focus-state').textContent, 'blurred');
  el.disconnectedCallback(); el.connectedCallback();
  const initial = el.actor.getSnapshot().context.text;
  await keys('Z'); assert.equal(area.value, `Z${initial}`, 'reconnect does not duplicate listeners');
});

test('editor element: Ctrl keys are normalized, Tab is not trapped, binding edits migrate', async (t) => {
  const el = mount('ta-editor');
  t.after(() => el.disconnectedCallback());
  let prevented = false;
  el.$('textarea').fire('keydown', { key: 'Tab', preventDefault: () => { prevented = true; } });
  assert.equal(prevented, false);
  el.$('.binding').value = 'j k l m'; el.$('.binding-form').fire('submit');
  await until(() => !el.upgrading, 'edited binding');
  for (const key of ['i', 'j', 'k', 'l', 'm']) { el.$('textarea').fire('keydown', { key }); el.system.flush(); }
  assert.equal(el.actor.getSnapshot().context.mode, 'normal');
  await el.upgrade({ keymap: 'emacs' });
  el.$('textarea').fire('keydown', { key: 'f', ctrlKey: true }); el.system.flush();
  assert.equal(el.actor.getSnapshot().context.cursor, 1);
  el.$('.binding').value = 'Escape'; el.$('.binding-form').fire('submit');
  assert.match(el.$('.migration').textContent, /2–32/);
});

test('elements: page polling remains live through the editor tests', () => {
  assert.equal(intervals.size, 1);
  for (const poll of intervals.values()) poll();
  assert.match(document.getElementById('stats').textContent, /^\d+ actors · \d+ hibernated · \d+ woken so far$/);
});
