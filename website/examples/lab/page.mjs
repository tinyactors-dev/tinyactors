// The lab's page: pick a scenario, a population and a datamodel, and watch
// one host in a Worker (worker.mjs) fill and work. The charts are the
// playgrounds' live costs (one point a second, the last minute), and a line
// under the controls says what the run has shown so far.
import { chart } from '/assets/components/costs.mjs';

const SCENARIOS = [
  ['fill', 'Fill a host', 'Spawn counters until the host holds them all, then hold them.'],
  ['events', 'Send events', 'Send events to counters drawn at random; each adds one.'],
  ['rally', 'Actors message actors', 'Pairs of actors send a ball back and forth, a hundred hits a rally.'],
  ['sleep', 'Most asleep', 'Keep 1,000 actors in memory and the rest in a store; each event wakes one.'],
];
const POPULATIONS = [1_000, 10_000, 100_000, 250_000, 500_000, 1_000_000, 2_000_000, 4_000_000];
const DEFAULT = 2; // 100,000
const number = new Intl.NumberFormat('en');
const megabytes = (bytes) => `${number.format(Math.round(bytes / 1048576))} MB`;
const rate = (v) => (v >= 1e6 ? `${(v / 1e6).toFixed(2)} M` : v >= 1e4 ? `${number.format(Math.round(v / 1e3))} k` : number.format(Math.round(v)));
const micro = (v) => `${v < 10 ? v.toFixed(2) : v.toFixed(1)} µs`;

export function mount(root) {
  let scenario = 'fill', datamodel = 'native';
  const scenarios = radios('Scenario', SCENARIOS.map(([name, label]) => [name, label]), scenario, (name) => { scenario = name; describe(); });
  const datamodels = radios('Datamodel', [['native', 'Native'], ['quickjs', 'quickjs']], datamodel, (name) => { datamodel = name; });
  const about = el('p', { class: 'lab__about' });
  const describe = () => { about.textContent = SCENARIOS.find(([name]) => name === scenario)[2]; };
  describe();
  const slider = el('input', { type: 'range', min: 0, max: POPULATIONS.length - 1, step: 1, value: DEFAULT, 'aria-label': 'Actors' });
  const count = el('output', { class: 'lab__count' });
  const showCount = () => { count.textContent = number.format(POPULATIONS[slider.value]); };
  slider.addEventListener('input', showCount);
  showCount();
  const run = el('button', { type: 'button', class: 'ta-btn ta-btn--primary' }, 'Start');
  const status = el('p', { class: 'lab__status', 'aria-live': 'polite' }, 'Pick a scenario and press Start. The host runs in a Worker on this device.');
  const found = el('p', { class: 'lab__found', 'aria-live': 'polite' });

  const costs = {
    actors: chart({ title: 'Actors', format: (v) => number.format(Math.round(v)) }),
    spawns: chart({ title: 'Spawns a second', format: rate }),
    memory: chart({ title: 'WebAssembly memory', format: megabytes }),
    perActor: chart({ title: 'Memory per actor', format: (v) => `${number.format(Math.round(v))} B` }),
    messages: chart({ title: 'Messages a second', format: rate }),
    perMessage: chart({ title: 'Time per message', format: micro, describe: (v) => `${v.toFixed(2)} microseconds` }),
    store: chart({ title: 'Store, asleep', format: megabytes }),
  };
  root.append(
    el('div', { class: 'lab__controls' }, scenarios, datamodels,
      el('label', { class: 'lab__population' }, 'Actors ', slider, count), run),
    about, status, found,
    el('div', { class: 'lab__costs' }, ...Object.values(costs).map((c) => c.element)),
  );

  let lab = null; // the running lab: its Worker and what it reported
  function stop(message) {
    lab?.worker.terminate(); // its WebAssembly memory goes with it
    lab = null;
    run.textContent = 'Start';
    if (message) status.textContent = message;
  }
  run.addEventListener('click', () => (lab ? stop('Stopped. Its memory was given back with its Worker.') : begin()));

  function begin() {
    const actors = POPULATIONS[slider.value];
    Object.values(costs).forEach((c) => c.clear());
    costs.actors.setLimit(actors, 'target');
    costs.store.element.hidden = scenario !== 'sleep';
    const worker = new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' });
    const current = (lab = { worker, scenario, datamodel, actors, began: performance.now(), filled: null, best: null });
    run.textContent = 'Stop';
    found.textContent = '';
    status.textContent = `Loading tinyactors…`;
    worker.onmessage = ({ data }) => {
      if (current !== lab) return;
      if (data.error) { stop(`The host stopped at ${number.format(current.last?.stats.actors ?? 0)} actors: ${data.error}`); return; }
      show(current, data);
    };
    worker.onerror = (event) => stop(`The host stopped: ${event.message}`);
    worker.postMessage({ start: true, scenario, actors, datamodel });
  }

  function show(current, data) {
    const { stats } = data;
    current.last = data;
    const used = data.memory !== null && data.base !== null ? data.memory - data.base : null;
    if (data.filled) {
      current.filled = { seconds: (performance.now() - current.began) / 1000, used, actors: stats.actors };
    } else {
      costs.actors.push(stats.actors);
      if (data.memory !== null) costs.memory.push(data.memory);
      if (data.phase === 'fill') costs.spawns.push(data.spawns);
      else {
        // Per actor only once the host is full: it reserves its records up
        // front, which a part of the population would make look larger.
        if (used !== null) costs.perActor.push(used / stats.actors);
        costs.messages.push(data.messages);
        if (data.perMessage !== null) costs.perMessage.push(data.perMessage);
        if (scenario === 'sleep') costs.store.push(stats.storeBytes);
        if (data.perMessage !== null && (current.best === null || data.messages > current.best.messages)) current.best = data;
      }
    }
    const engine = current.datamodel === 'native' ? 'native datamodel' : 'quickjs';
    if (data.phase === 'fill') {
      status.textContent = `Spawning ${number.format(stats.target)} actors on the ${engine}… ${number.format(stats.actors)}`;
      return;
    }
    status.textContent = data.phase === 'holding'
      ? `Holding ${number.format(stats.actors)} actors, every one waiting for an event.`
      : `Running: ${number.format(stats.messages)} messages so far${current.scenario === 'sleep' ? `, ${number.format(stats.wakes)} actors woken from the store` : ''}.`;
    found.textContent = summary(current);
  }

  // What the run has shown: what fit, how fast it filled, the best second.
  function summary({ filled, best, datamodel: model }) {
    if (!filled) return '';
    const parts = [];
    const each = filled.used !== null ? `, ${number.format(Math.round(filled.used / filled.actors))} B each` : '';
    parts.push(`${number.format(filled.actors)} actors fit in ${filled.used !== null ? megabytes(filled.used) : 'this host'}${each}, spawned in ${filled.seconds.toFixed(1)} s.`);
    if (best) parts.push(`Best second: ${rate(best.messages)} messages, ${micro(best.perMessage)} each.`);
    if (model === 'native') parts.push('On the native datamodel, actors’ variables live in the JavaScript heap, which this memory leaves out.');
    return parts.join(' ');
  }
}

/** A row of mutually exclusive choices; `pick(name)` on each change. */
function radios(label, choices, initial, pick) {
  const group = el('div', { role: 'radiogroup', class: 'ta-tabs ta-tabs--segmented', 'aria-label': label });
  for (const [name, text] of choices) {
    const button = el('button', { type: 'button', role: 'radio', class: 'ta-tab', 'aria-checked': String(name === initial) }, text);
    button.addEventListener('click', () => {
      for (const other of group.children) other.setAttribute('aria-checked', String(other === button));
      pick(name);
    });
    group.append(button);
  }
  return group;
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
