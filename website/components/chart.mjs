// A chart's diagram: when its "Diagram" panel is shown (tabs.mjs dispatches
// `ta-panel-shown`), an <scxml-view> of @tinyactors/scxmljs is made in it
// for the chart at its data-src, once. The view runs the chart in its
// QuickJS sandbox, highlights the active states, and sends an event when a
// transition's label is clicked. Its module and QuickJS load with the first
// diagram only. A panel already shown when this runs (a choice remembered
// from an earlier page, applied by tabs.mjs before this loaded) is made too.
//
// The view runs the chart as a demo on its own: a stand-in for each I/O
// processor the chart sends to (the reader sees what the payment service
// was asked, and plays it by clicking the answer's label), its variables
// after each step, a restart button, and a form for an event that takes
// data. A program's diagram may say more (its <diagram> in the content):
// the panel's data-event-data is what a label's click sends for each event
// ({"add": {"item": "pen", "price": 12}}), and data-events the buttons
// under the diagram that send an event with its data ([{name, data,
// label}]: "Add a pen"). A small chart grows to fill the panel
// (--scxml-max-scale, style/site.css).

async function mount(panel) {
  if (!panel.matches('.chart__diagram[data-src]') || panel.querySelector('scxml-view')) return;
  await import('@tinyactors/scxmljs/view');
  if (panel.querySelector('scxml-view')) return;
  const view = document.createElement('scxml-view');
  view.setAttribute('src', panel.dataset.src);
  view.setAttribute('fit', '');
  view.setAttribute('variables', '');
  view.setAttribute('warnings', 'off');
  if (panel.dataset.eventData) view.setAttribute('event-data', panel.dataset.eventData);
  const sends = JSON.parse(panel.dataset.events ?? '[]').map(({ name, data, label }) => ({ event: name, data, label }));
  if (sends.length) view.setAttribute('sends', JSON.stringify(sends));
  panel.append(view);
}

document.addEventListener('ta-panel-shown', (event) => mount(event.target));
// The same URL as the page's <script> for tabs.mjs (its ?v= query), so one module defines <ta-tabs>.
await import(new URL(`./tabs.mjs${new URL(import.meta.url).search}`, import.meta.url));
for (const panel of document.querySelectorAll('.chart__diagram[data-src]:not([hidden])')) mount(panel);
