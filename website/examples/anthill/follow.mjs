// The panel of the ant a reader follows: its chart, drawn by scxmljs's
// <scxml-view>, with the states the ant is in lit. The ant runs in a
// colony's host, not in the view: the view is handed a mirror of a session
// (the properties and events <scxml-view> reads of one), and each report of
// the worker (world.mjs, inspect()) moves the mirror to the state value core
// gave (actor.state). The transitions that fired are worked out from the
// states it left and entered and the events the ant received.
import { compile, nameMatch, parseSCXML } from '@tinyactors/scxmljs';
import '@tinyactors/scxmljs/view';

const KEEP = 6; // the newest events the panel lists
const number = new Intl.NumberFormat('en');

/** A session's face, as <scxml-view> reads it, showing states it is told of. */
class Mirror extends EventTarget {
  constructor(model) {
    super();
    this.model = model;
    this.status = 'running';
    this.clock = { now: () => performance.now() };
    this.signal = new AbortController().signal;
    this.active = new Set();
  }
  get configuration() { return [...this.active]; }
  isActiveNode(node) { return this.active.has(node); }
  start() {}
  send() {}
  dispose() {}

  /** The ant is in the states of `next` (StateNodes), after receiving `received` (event names). */
  move(next, received) {
    const before = this.active;
    const exited = [...before].filter((node) => !next.has(node));
    const entered = [...next].filter((node) => !before.has(node)).sort((a, b) => a.depth - b.depth);
    const matches = (transition) => transition.events.length > 0 && received.some((event) => nameMatch(transition.events, event));
    const possible = this.model.states.flatMap((state) => state.transitions).filter((t) => before.has(t.source));
    // A transition that entered a state: the one the received events name,
    // or an eventless one, else any that could have.
    const changed = possible.filter((t) => t.targets.some((target) => entered.includes(target)));
    const named = changed.filter((t) => matches(t) || t.events.length === 0);
    // A received event that left the ant where it was: a targetless
    // transition, or one back into a state it is still in.
    const stayed = possible.filter((t) => matches(t) && t.targets.every((target) => next.has(target)) && !changed.includes(t));
    const transitions = [...(named.length ? named : changed), ...stayed];
    this.active = next;
    this.dispatchEvent(Object.assign(new Event('microstep'), { transitions, entered, exited }));
    this.dispatchEvent(Object.assign(new Event('macrostep'), { event: received.length ? { name: received.at(-1) } : null }));
  }

  /** The ant ended in final state `final`. */
  end(final, received) {
    this.move(new Set(final ? [final] : []), received);
    this.status = 'done';
    this.dispatchEvent(new Event('done'));
  }
}

/** The states of actor.state's value, as nodes of `model`: each key a state, each string an atomic one. */
function nodesOf(value, model, into = new Set()) {
  const add = (id) => {
    const node = model.byId.get(id);
    if (node) into.add(node);
  };
  if (typeof value === 'string') add(value);
  else if (value && typeof value === 'object') {
    for (const [id, inner] of Object.entries(value)) {
      add(id);
      nodesOf(inner, model, into);
    }
  }
  return into;
}

/** The leaf path of a state value, as "worker › foraging". */
function path(value) {
  const names = [];
  while (value && typeof value === 'object' && Object.keys(value).length === 1) {
    const [id] = Object.keys(value);
    names.push(id);
    value = value[id];
  }
  if (typeof value === 'string') names.push(value);
  return names.join(' › ');
}

/**
 * The panel for the chart in `text`. `onStop` and `onAnother` are its two
 * buttons'. Answers {element, follow(slot, colony), show(report), hide()}.
 */
export function followPanel(text, { onStop, onAnother }) {
  const model = compile(parseSCXML(text));
  const title = el('span', { class: 'anthill__follow-title' });
  const stop = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Stop following');
  stop.addEventListener('click', onStop);
  const another = el('button', { type: 'button', class: 'ta-btn ta-btn--primary ta-btn--sm', hidden: '' }, 'Follow another ant');
  another.addEventListener('click', onAnother);
  const view = el('scxml-view', { interactive: 'false', announce: 'off', warnings: 'off' });
  const frame = el('div', { class: 'anthill__follow-chart' }, view);
  view.addEventListener('scxml-load', () => fit(view, frame));
  new ResizeObserver(() => fit(view, frame)).observe(frame);
  const state = el('p', { class: 'anthill__follow-state', 'aria-live': 'polite' });
  const log = el('ol', { class: 'anthill__follow-events' });
  const element = el('aside', { class: 'anthill__follow', 'aria-label': 'The ant you follow', hidden: '' },
    el('div', { class: 'anthill__follow-head' }, title, another, stop), frame, state, log);
  let mirror = null, events = [];

  return {
    element,
    /** Starts showing the ant in `slot` of colony `colony`. */
    follow(slot, colony) {
      mirror = new Mirror(model);
      view.session = mirror;
      events = [];
      title.textContent = `Ant ${number.format(slot + 1)}, hill ${colony + 1}`;
      state.textContent = 'Asking its host…';
      log.replaceChildren();
      another.hidden = true;
      element.hidden = false;
      fit(view, frame);
    },
    /** A worker's report of the followed ant (world.mjs, inspect()). */
    show(report) {
      if (!mirror || mirror.status === 'done') return;
      const received = report.events.filter((event) => !event.sent).map((event) => event.event);
      events.push(...report.events);
      events = events.slice(-KEEP);
      log.replaceChildren(...events.slice().reverse().map((event) =>
        el('li', {}, event.sent ? 'sent ' : 'received ', el('code', {}, event.event))));
      if (report.ended) {
        mirror.end(model.states.find((node) => node.kind === 'final' && node.parent === model.root), received);
        state.textContent = 'This ant died.';
        another.hidden = false;
        return;
      }
      mirror.move(nodesOf(report.value, model), received);
      const hp = report.variables?.hp;
      state.replaceChildren(el('code', {}, path(report.value)),
        ...(typeof hp === 'number' ? [' · ', el('code', {}, `hp = ${hp}`)] : []),
        report.outside ? '' : ' · in the hill');
    },
    hide() {
      element.hidden = true;
      mirror = null;
    },
  };
}

/**
 * Scales the whole diagram (CSS zoom, which scales its layout box too) to
 * fit `frame`, so every state of the ant is in sight at once.
 */
async function fit(view, frame) {
  let svg = null;
  for (let tries = 0; tries < 120 && !svg; tries++) {
    svg = view.shadowRoot?.querySelector('[part~="state"]') ? view.shadowRoot.querySelector('svg') : null;
    if (!svg) await new Promise((resolve) => requestAnimationFrame(resolve));
  }
  const width = Number(svg?.getAttribute('width')), height = Number(svg?.getAttribute('height'));
  if (!width || !height || !frame.clientWidth) return;
  const room = { width: frame.clientWidth - 16, height: frame.clientHeight - 16 };
  const scale = Math.min(1, room.width / width, room.height > 80 ? room.height / height : 1);
  view.style.zoom = String(Math.floor(scale * 100) / 100);
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
