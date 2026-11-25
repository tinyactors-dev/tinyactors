// A chart's diagram: when its "Diagram" panel is shown (tabs.mjs dispatches
// `ta-panel-shown`), an <scxml-view> of @tinyactors/scxmljs is made in it
// for the chart at its data-src, once. The view runs the chart in its
// QuickJS sandbox, highlights the active states, and sends an event when a
// transition's label is clicked. Its module and QuickJS load with the first
// diagram only. A panel already shown when this runs (a choice remembered
// from an earlier page, applied by tabs.mjs before this loaded) is made too.
//
// The view's session knows only SCXML's own I/O processor, and a chart that
// sends to the world (<send type="urn:payments">) would fail there. So each
// type a chart sends to gets a stand-in, which keeps what it receives and
// shows it under the diagram: the reader sees what the payment service was
// asked, and plays the service by clicking the answer's label (charged).
// The chart's variables are listed beside them, after each step.
//
// A program's diagram may say more (its <diagram> in the content): the
// panel's data-event-data is what a label's click sends for each event
// ({"add": {"item": "pen", "price": 12}}), and data-events the buttons
// under the diagram that send an event with its data ([{name, data,
// label}]: "Add a pen").

const SCXML = new Set(['scxml', 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor']);
const KEEP = 20; // the newest messages a stand-in lists

/** The I/O processor types a chart's <send type="..."> elements name. */
function sendTypes(text) {
  const types = [...text.matchAll(/<send\b[^>]*\btype="([^"]+)"/g)].map((match) => match[1]);
  return [...new Set(types)].filter((type) => !SCXML.has(type));
}

/** A stand-in for the processor of `type`, and the panel that shows what it received. */
function standIn(type) {
  const received = [];
  let started = performance.now();
  const count = el('span', { class: 'processor__count' });
  const log = el('ol', { class: 'processor__log', 'aria-live': 'polite' });
  const panel = el('section', { class: 'processor', 'aria-label': `What ${type} received` },
    el('p', { class: 'processor__head' }, el('code', { class: 'processor__type' }, type), count), log);

  function render() {
    count.textContent = received.length === 1 ? '1 message received' : `${received.length} messages received`;
    log.replaceChildren(...received.slice(-KEEP).reverse().map((message) => el('li', {},
      el('span', { class: 'processor__at' }, clock(message.at)),
      el('code', {}, message.event),
      ...(message.target ? [' to ', el('code', {}, message.target)] : []),
      ...(shown(message.data) ? [' ', el('code', { class: 'processor__data' }, shown(message.data))] : []))));
    if (received.length === 0) log.append(el('li', { class: 'processor__empty' }, 'Nothing yet.'));
  }
  render();
  return {
    panel,
    /** A new run of the chart: the stand-in starts empty. */
    restart() {
      received.length = 0;
      started = performance.now();
      render();
    },
    processor: {
      type,
      location: (session) => `${type}#${session.sessionId}`,
      send(message) {
        received.push({ at: performance.now() - started, event: message.event, target: message.target, data: message.data });
        render();
      },
    },
  };
}

/** The chart's variables, listed after each step of the session it watches. */
function variables() {
  const list = el('dl', { class: 'variables__list', 'aria-live': 'polite' });
  const panel = el('section', { class: 'processor variables', 'aria-label': 'Variables' },
    el('p', { class: 'processor__head' }, el('span', {}, 'Variables')), list);
  let session = null;
  const render = () => {
    const entries = Object.entries(session?.snapshot() ?? {});
    list.replaceChildren(...entries.flatMap(([name, value]) => [
      el('dt', {}, el('code', {}, name)), el('dd', {}, el('code', { class: 'processor__data' }, JSON.stringify(value) ?? 'undefined'))]));
    panel.hidden = entries.length === 0;
  };
  return {
    panel,
    /** Lists `next`'s variables from now on, after each of its macrosteps. */
    watch(next) {
      session = next;
      session.addEventListener('macrostep', render);
      render();
    },
  };
}

/** Milliseconds as m:ss. */
function clock(ms) {
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}

/** A message's data as JSON, or nothing when it has none. */
function shown(data) {
  if (data === undefined || data === null) return '';
  const text = JSON.stringify(data);
  return text === '{}' ? '' : text;
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}

async function mount(panel) {
  if (!panel.matches('.chart__diagram[data-src]') || panel.querySelector('scxml-view')) return;
  const src = panel.dataset.src;
  const [text] = await Promise.all([fetch(src).then((response) => response.text()), import('@tinyactors/scxmljs/view')]);
  if (panel.querySelector('scxml-view')) return;
  const view = document.createElement('scxml-view');
  const standIns = sendTypes(text).map(standIn);
  const data = variables();
  let session = null;
  view.options = { ioprocessors: standIns.map((stand) => stand.processor) };
  view.addEventListener('scxml-load', (event) => {
    session = event.detail.session;
    standIns.forEach((stand) => stand.restart());
    data.watch(session);
  });
  view.setAttribute('src', src);
  view.setAttribute('fit', '');
  view.setAttribute('warnings', 'off');
  if (panel.dataset.eventData) view.setAttribute('event-data', panel.dataset.eventData);
  view.addEventListener('scxml-load', () => enlarge(view, panel), { once: true });
  panel.append(view);
  const again = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Run again');
  again.addEventListener('click', () => view.reload());
  const sends = JSON.parse(panel.dataset.events ?? '[]').map(({ name, data: payload, label }) => {
    const button = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, label);
    button.addEventListener('click', () => session?.send(name, payload));
    return button;
  });
  panel.append(el('div', { class: 'processors' }, data.panel, ...standIns.map((stand) => stand.panel)));
  panel.append(el('p', { class: 'chart__controls' }, ...sends, again));
}

// The view draws a chart at its natural size: a small chart is a few
// hundred pixels in the corner of a wide panel. Scale the whole element
// with CSS zoom, which scales its layout box too (the view's own zoom
// scrolls its canvas instead), so the drawing fills the panel's width or
// --chart-height (style/site.css), at most 1.5 times.
async function enlarge(view, panel) {
  let svg = null;
  for (let frame = 0; frame < 120; frame++) {
    svg = view.shadowRoot?.querySelector('[part~="state"]') ? view.shadowRoot.querySelector('svg') : null;
    if (svg) break;
    await new Promise((resolve) => requestAnimationFrame(resolve));
  }
  const width = Number(svg?.getAttribute('width'));
  const height = Number(svg?.getAttribute('height'));
  if (!width || !height) return;
  const room = { width: panel.clientWidth - 64, height: parseFloat(getComputedStyle(panel).getPropertyValue('--chart-height')) || 320 };
  const scale = Math.max(1, Math.min(1.5, room.width / width, room.height / height));
  view.style.zoom = String(Math.round(scale * 100) / 100);
}

document.addEventListener('ta-panel-shown', (event) => mount(event.target));
// The same URL as the page's <script> for tabs.mjs (its ?v= query), so one module defines <ta-tabs>.
await import(new URL(`./tabs.mjs${new URL(import.meta.url).search}`, import.meta.url));
for (const panel of document.querySelectorAll('.chart__diagram[data-src]:not([hidden])')) mount(panel);
