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
// type a chart sends to gets a stand-in that takes the send and stays
// silent: the reader plays the world, by clicking the label of the answer
// (charged) on the diagram.

const SCXML = new Set(['scxml', 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor']);

/** The I/O processor types a chart's <send type="..."> elements name. */
function sendTypes(text) {
  const types = [...text.matchAll(/<send\b[^>]*\btype="([^"]+)"/g)].map((match) => match[1]);
  return [...new Set(types)].filter((type) => !SCXML.has(type));
}

function standIn(type) {
  return { type, location: (session) => `${type}#${session.sessionId}`, send() {} };
}

async function mount(panel) {
  if (!panel.matches('.chart__diagram[data-src]') || panel.querySelector('scxml-view')) return;
  const src = panel.dataset.src;
  const [text] = await Promise.all([fetch(src).then((response) => response.text()), import('@tinyactors/scxmljs/view')]);
  if (panel.querySelector('scxml-view')) return;
  const view = document.createElement('scxml-view');
  view.options = { ioprocessors: sendTypes(text).map(standIn) };
  view.setAttribute('src', src);
  view.setAttribute('fit', '');
  view.setAttribute('warnings', 'off');
  view.addEventListener('scxml-load', () => enlarge(view, panel), { once: true });
  panel.append(view);
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
