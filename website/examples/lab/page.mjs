// The lab's page: pick a scenario, how the population moves, a peak and a
// datamodel, and watch one host in a Worker (worker.mjs) follow it. The
// timeline (components/timeline.mjs) charts the whole run, a point a
// second, and a line under the controls says what the run has shown.
import { timeline } from '/assets/components/timeline.mjs';

const SCENARIOS = [
  ['idle', 'Idle actors', 'Every actor waits for an event.'],
  ['events', 'Send events', 'Counters drawn at random get events and each adds one.'],
  ['rally', 'Actors message actors', 'Pairs of actors send a ball back and forth, a hundred hits a rally.'],
  ['sleep', 'Most asleep', '1,000 actors stay in memory and the rest sleep in a store; each event wakes one.'],
  ['timers', 'Timers', 'Every actor holds a delayed send to itself, due every 1 to 10 simulated seconds.'],
];
const PATTERNS = [
  ['steady', 'Steady', 'The population fills to its peak and stays there.'],
  ['ramp', 'Ramp', 'The population climbs from a tenth to its peak in a minute, then falls back.'],
  ['waves', 'Waves', 'The population rises and falls once a minute, from a tenth to its peak.'],
  ['bursts', 'Bursts', 'A fifth of the peak, and the whole peak for 10 seconds out of every 40.'],
];
const POPULATIONS = [1_000, 10_000, 100_000, 250_000, 500_000, 1_000_000, 2_000_000, 4_000_000];
const DEFAULT = 2; // 100,000
const number = new Intl.NumberFormat('en');
const count = (v) => (v >= 1e6 ? `${(v / 1e6).toFixed(2)} M` : v >= 1e4 ? `${number.format(Math.round(v / 1e3))} k` : number.format(Math.round(v)));
const megabytes = (bytes) => `${number.format(Math.round(bytes / 1048576))} MB`;
const micro = (v) => `${v < 10 ? v.toFixed(2) : v.toFixed(1)} µs`;

export function mount(root) {
  root.classList.add('lab');
  let scenario = 'events', pattern = 'waves', datamodel = 'native';
  const about = el('p', { class: 'lab__about' });
  const describe = () => {
    about.textContent = `${SCENARIOS.find(([name]) => name === scenario)[2]} ${PATTERNS.find(([name]) => name === pattern)[2]}`;
  };
  const scenarios = cards('Scenario', SCENARIOS, scenario, (name) => { scenario = name; describe(); });
  const patterns = radios('Population over time', PATTERNS, pattern, (name) => { pattern = name; describe(); });
  const datamodels = radios('Datamodel', [['native', 'Native'], ['quickjs', 'quickjs']], datamodel, (name) => { datamodel = name; });
  describe();
  const slider = el('input', { type: 'range', min: 0, max: POPULATIONS.length - 1, step: 1, value: DEFAULT, 'aria-label': 'Peak actors' });
  const peak = el('output', { class: 'lab__count' });
  const showPeak = () => { peak.textContent = number.format(POPULATIONS[slider.value]); };
  slider.addEventListener('input', showPeak);
  showPeak();
  const run = el('button', { type: 'button', class: 'ta-btn ta-btn--primary lab__run' }, 'Start');
  const status = el('p', { class: 'lab__status', 'aria-live': 'polite' }, 'The host runs in a Worker on this device.');
  const found = el('p', { class: 'lab__found', 'aria-live': 'polite' });
  const charts = el('div', { class: 'lab__charts' });

  root.append(
    scenarios,
    el('div', { class: 'lab__controls' },
      el('div', { class: 'lab__field' }, el('span', { class: 'lab__label' }, 'Population over time'), patterns),
      el('div', { class: 'lab__field' }, el('span', { class: 'lab__label' }, 'Datamodel'), datamodels),
      el('label', { class: 'lab__field lab__peak' }, el('span', { class: 'lab__label' }, 'Peak actors'), el('span', { class: 'lab__slider' }, slider, peak)),
      run),
    about, status, found, charts,
  );

  let lab = null; // the running lab: its Worker, its timeline and what it found
  function stop(message) {
    lab?.worker.terminate(); // its WebAssembly memory goes with it
    lab = null;
    run.textContent = 'Start';
    if (message) status.textContent = message;
  }
  run.addEventListener('click', () => (lab ? stop('Stopped. Its memory was given back with its Worker.') : begin()));

  function begin() {
    const actors = POPULATIONS[slider.value];
    const sleep = scenario === 'sleep';
    const rate = scenario === 'timers' ? 'Timers fired a second' : 'Messages a second';
    const view = timeline({ panels: [
      { id: 'actors', title: 'Actors', format: count,
        series: [{ key: 'actors', label: 'alive' }, ...(sleep ? [{ key: 'asleep', label: 'asleep in the store' }] : [])],
        reference: { key: 'target', label: 'pattern' } },
      { id: 'churn', title: 'Spawned and ended a second', format: count,
        series: [{ key: 'spawns', label: 'spawned' }, { key: 'ends', label: 'ended' }] },
      ...(scenario === 'idle' ? [] : [
        { id: 'messages', title: rate, format: count, series: [{ key: 'messages', label: rate }] },
        { id: 'perMessage', title: 'Time per message', format: micro, series: [{ key: 'perMessage', label: 'Time per message' }] },
      ]),
      { id: 'memory', title: 'Memory', format: megabytes,
        series: [{ key: 'memory', label: 'WebAssembly' }, ...(sleep ? [{ key: 'store', label: 'the store' }] : [])] },
    ] });
    charts.replaceChildren(view.element);
    const worker = new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' });
    const current = (lab = { worker, view, scenario, datamodel, pattern, base: null, peak: null, best: null, last: null });
    run.textContent = 'Stop';
    found.textContent = '';
    status.textContent = 'Loading tinyactors…';
    worker.onmessage = ({ data }) => {
      if (current !== lab) return;
      if (data.error) { stop(`The host stopped at ${number.format(current.last?.stats.actors ?? 0)} actors: ${data.error}`); return; }
      if (data.ready) { current.base = data.base; status.textContent = 'Spawning…'; return; }
      show(current, data);
    };
    worker.onerror = (event) => stop(`The host stopped: ${event.message}`);
    worker.postMessage({ start: true, scenario, actors, datamodel, pattern });
  }

  function show(current, data) {
    const { stats } = data;
    current.last = data;
    current.view.push(data.second, {
      actors: stats.actors, target: data.target, asleep: stats.asleep,
      spawns: data.spawns, ends: data.ends,
      messages: data.messages, perMessage: data.perMessage,
      memory: data.memory, store: stats.storeBytes,
    });
    const used = data.memory !== null && current.base !== null ? data.memory - current.base : null;
    if (stats.actors > 0 && (current.peak === null || stats.actors > current.peak.actors)) current.peak = { actors: stats.actors, used };
    if (data.perMessage !== null && (current.best === null || data.messages > current.best.messages)) current.best = data;
    status.textContent = `${number.format(stats.actors)} actors alive. ${number.format(stats.spawned)} spawned and ${number.format(stats.ended)} ended in ${data.second} s`
      + (current.scenario === 'idle' ? '.' : `; ${number.format(stats.messages)} ${current.scenario === 'timers' ? 'timers fired' : 'messages'}.`);
    found.textContent = summary(current);
  }

  // What the run has shown: the most actors at once and their memory, the best second.
  function summary({ peak, best, datamodel: model, scenario: kind }) {
    if (!peak) return '';
    const parts = [];
    const each = peak.used !== null ? ` in ${megabytes(peak.used)}, ${number.format(Math.round(peak.used / peak.actors))} B each` : '';
    parts.push(`Most at once: ${number.format(peak.actors)} actors${each}.`);
    if (best) parts.push(`Best second: ${count(best.messages)} ${kind === 'timers' ? 'timers fired' : 'messages'}, ${micro(best.perMessage)} each.`);
    parts.push('WebAssembly memory keeps its peak when actors end.');
    if (model === 'native') parts.push('On the native datamodel, actors’ variables live in the JavaScript heap, which this memory leaves out.');
    return parts.join(' ');
  }
}

/** The scenarios, as cards: a name and what happens, one picked at a time. */
function cards(label, choices, initial, pick) {
  const group = el('div', { role: 'radiogroup', class: 'lab__scenarios', 'aria-label': label });
  for (const [name, text, description] of choices) {
    const button = el('button', { type: 'button', role: 'radio', class: 'lab__scenario', 'aria-checked': String(name === initial) },
      el('span', { class: 'lab__scenario-name' }, text), el('span', { class: 'lab__scenario-about' }, description));
    button.addEventListener('click', () => {
      for (const other of group.children) other.setAttribute('aria-checked', String(other === button));
      pick(name);
    });
    group.append(button);
  }
  return group;
}

/** A row of mutually exclusive choices; `pick(name)` on each change. */
function radios(label, choices, initial, pick) {
  const group = el('div', { role: 'radiogroup', class: 'ta-tabs ta-tabs--segmented lab__choices', 'aria-label': label });
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
