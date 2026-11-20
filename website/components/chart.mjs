// A chart's diagram: when its "Diagram" panel is shown (tabs.mjs dispatches
// `ta-panel-shown`), an <scxml-view> of @tinyactors/scxmljs is made in it
// for the chart at its data-src, once. The view runs the chart in its
// QuickJS sandbox, highlights the active states, and sends an event when a
// transition's label is clicked. Its module and QuickJS load with the first
// diagram only. A panel already shown when this runs (a choice remembered
// from an earlier page, applied by tabs.mjs before this loaded) is made too.

async function mount(panel) {
  if (!panel.matches('.chart__diagram[data-src]') || panel.querySelector('scxml-view')) return;
  const view = document.createElement('scxml-view');
  view.setAttribute('src', panel.dataset.src);
  view.setAttribute('fit', '');
  view.setAttribute('warnings', 'off');
  panel.append(view);
  await import('@tinyactors/scxmljs/view');
}

document.addEventListener('ta-panel-shown', (event) => mount(event.target));
// The same URL as the page's <script> for tabs.mjs (its ?v= query), so one module defines <ta-tabs>.
await import(new URL(`./tabs.mjs${new URL(import.meta.url).search}`, import.meta.url));
for (const panel of document.querySelectorAll('.chart__diagram[data-src]:not([hidden])')) mount(panel);
