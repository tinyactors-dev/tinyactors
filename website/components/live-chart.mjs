// A chart drawn by scxmljs's <scxml-view>, lit in the states of an actor
// that runs somewhere else: a tinyactors host, in a Worker or on this page.
// The view is handed a mirror of a session (the properties and events
// <scxml-view> reads of one), and show() moves the mirror to the state value
// the host reported (actor.state). The transitions that fired are worked
// out from the states it left and entered and the events the actor
// received, when the caller knows them. Used by the anthill's follow mode
// and the live chat.
import { compile, nameMatch, parseSCXML } from '@tinyactors/scxmljs';
import '@tinyactors/scxmljs/view';

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

  /** The actor is in the states of `next` (StateNodes), after receiving `received` (event names). */
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
    // A received event that left the actor where it was: a targetless
    // transition, or one back into a state it is still in.
    const stayed = possible.filter((t) => matches(t) && t.targets.every((target) => next.has(target)) && !changed.includes(t));
    const transitions = [...(named.length ? named : changed), ...stayed];
    this.active = next;
    this.dispatchEvent(Object.assign(new Event('microstep'), { transitions, entered, exited }));
    this.dispatchEvent(Object.assign(new Event('macrostep'), { event: received.length ? { name: received.at(-1) } : null }));
  }

  /** The actor ended in final state `final`. */
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
export function statePath(value) {
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
 * The chart in `text`, drawn in `frame` (an element whose size the diagram
 * is scaled to fit). Answers {view, restart(), show(value, received), end(received)}.
 */
export function liveChart(text, frame) {
  const model = compile(parseSCXML(text));
  const view = document.createElement('scxml-view');
  for (const [name, value] of Object.entries({ interactive: 'false', announce: 'off', warnings: 'off' })) view.setAttribute(name, value);
  frame.append(view);
  view.addEventListener('scxml-load', () => fit(view, frame));
  new ResizeObserver(() => fit(view, frame)).observe(frame);
  let mirror = null;
  const chart = {
    view,
    /** A new actor: nothing lit until show(). */
    restart() {
      mirror = new Mirror(model);
      view.session = mirror;
      fit(view, frame);
    },
    /** The actor is in `value` (actor.state's), after receiving the events named in `received`. */
    show(value, received = []) {
      if (!mirror || mirror.status === 'done') return;
      mirror.move(nodesOf(value, model), received);
    },
    /** The actor ended, in the chart's top-level final state. */
    end(received = []) {
      if (!mirror || mirror.status === 'done') return;
      mirror.end(model.states.find((node) => node.kind === 'final' && node.parent === model.root), received);
    },
    get ended() { return mirror?.status === 'done'; },
  };
  chart.restart();
  return chart;
}

/**
 * Scales the whole diagram (CSS zoom, which scales its layout box too) to
 * fit `frame`, so every state is in sight at once.
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
