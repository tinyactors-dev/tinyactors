// The page's live examples: each export is what a button on the page runs,
// in the reader's browser, on @tinyactors/wasm. It answers the lines to
// show and, for a traced run, its spans for the trace view.
import { broken, compile, run, search, shrink } from './property.mjs';

const chart = fetch(new URL('./payment.scxml', import.meta.url)).then((response) => response.text());
const buggy = async () => compile((await chart).replace('attempts &lt; 3', 'attempts &lt; 4'));
const show = (answers) => `[${answers.map((yes) => (yes ? 'answer' : 'silence')).join(', ')}]`;

// One run, traced: the service is silent once, then answers.
export async function trace() {
  const compiled = compile(await chart);
  const r = await run(compiled, [false, true], { trace: true });
  compiled.close();
  return { lines: [`${show(r.answers)}: ${r.charges.length} charges, ${r.outcome}`], spans: r.spans };
}

// A thousand worlds against the chart as it is.
export async function correct() {
  const compiled = compile(await chart);
  const started = performance.now();
  const found = await search(compiled, 1000);
  compiled.close();
  const ms = Math.round(performance.now() - started);
  return { lines: [found ? `seed ${found.seed} breaks ${broken(found.run).join(', ')}` : `1000 worlds: every property held (${ms} ms)`] };
}

// The same search against a chart with one retry too many.
export async function find() {
  const compiled = await buggy();
  const failure = await search(compiled, 1000);
  compiled.close();
  return { lines: [`seed ${failure.seed}: ${show(failure.run.answers)} breaks ${broken(failure.run).join(', ')}`] };
}

// That failure shrunk, and the smallest one replayed with its trace.
export async function counterexample() {
  const compiled = await buggy();
  const failure = await search(compiled, 1000);
  const smallest = await shrink(compiled, failure.run.answers);
  const replay = await run(compiled, smallest, { trace: true });
  compiled.close();
  return {
    lines: [`seed ${failure.seed} shrunk to ${show(smallest)}: ${replay.charges.length} charges, ${replay.outcome}`],
    spans: replay.spans,
  };
}
