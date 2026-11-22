// The panel of the ant a reader follows: its chart, lit in the states the
// ant is in (components/live-chart.mjs), with each report of the worker
// (world.mjs, inspect()): the state value core gave (actor.state), the
// events the ant sent and received, its hp.
import { liveChart, statePath } from '/assets/components/live-chart.mjs';

const KEEP = 6; // the newest events the panel lists
const number = new Intl.NumberFormat('en');

/**
 * The panel for the chart in `text`. `onStop` and `onAnother` are its two
 * buttons'. Answers {element, follow(slot, colony), show(report), hide()}.
 */
export function followPanel(text, { onStop, onAnother }) {
  const title = el('span', { class: 'anthill__follow-title' });
  const stop = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Stop following');
  stop.addEventListener('click', onStop);
  const another = el('button', { type: 'button', class: 'ta-btn ta-btn--primary ta-btn--sm', hidden: '' }, 'Follow another ant');
  another.addEventListener('click', onAnother);
  const frame = el('div', { class: 'anthill__follow-chart' });
  const state = el('p', { class: 'anthill__follow-state', 'aria-live': 'polite' });
  const log = el('ol', { class: 'anthill__follow-events' });
  const element = el('aside', { class: 'anthill__follow', 'aria-label': 'The ant you follow', hidden: '' },
    el('div', { class: 'anthill__follow-head' }, title, another, stop), frame, state, log);
  const chart = liveChart(text, frame);
  let events = [], following = false;

  return {
    element,
    /** Starts showing the ant in `slot` of colony `colony`. */
    follow(slot, colony) {
      element.hidden = false;
      chart.restart();
      following = true;
      events = [];
      title.textContent = `Ant ${number.format(slot + 1)}, hill ${colony + 1}`;
      state.textContent = 'Asking its host…';
      log.replaceChildren();
      another.hidden = true;
    },
    /** A worker's report of the followed ant (world.mjs, inspect()). */
    show(report) {
      if (!following || chart.ended) return;
      const received = report.events.filter((event) => !event.sent).map((event) => event.event);
      events.push(...report.events);
      events = events.slice(-KEEP);
      log.replaceChildren(...events.slice().reverse().map((event) =>
        el('li', {}, event.sent ? 'sent ' : 'received ', el('code', {}, event.event))));
      if (report.ended) {
        chart.end(received);
        state.textContent = 'This ant died.';
        another.hidden = false;
        return;
      }
      chart.show(report.value, received);
      const hp = report.variables?.hp;
      state.replaceChildren(el('code', {}, statePath(report.value)),
        ...(typeof hp === 'number' ? [' · ', el('code', {}, `hp = ${hp}`)] : []),
        report.outside ? '' : ' · in the hill');
    },
    hide() {
      element.hidden = true;
      following = false;
    },
  };
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
