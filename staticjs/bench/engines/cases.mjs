// The cases `mise run bench-engines` compares, and the size each runs at:
// the one list to edit. Each name is a case of bench/bench.c, which defines
// its source, globals, fixtures and expected value (dump.c reads them from
// there); n must be one of that case's sizes. A compile case has no size:
// it compiles its module, whose length in bytes the report gives.
//
// A case can be listed only when guest.mjs can express it in plain
// JavaScript: an eval case whose fixtures guest.js generates, or a compile
// case of a module (bench/modules/). Start-up rows are in compare.mjs.
export const CASES = [
  ['compile.module.power.whole'],
  ['compile.module.campaign.whole'],
  ['eval.guard.event', 1],
  ['eval.guard.window', 1],
  ['eval.assign.counter', 1],
  ['eval.send.target', 1],
  ['eval.module.power', 1],
  ['eval.call.waterFill', 300],
  ['eval.array.map', 10000],
  ['eval.array.filter', 10000],
  ['eval.array.reduce', 10000],
  ['eval.array.pipeline', 10000],
  ['eval.array.toSorted', 10000],
  ['eval.array.toSorted.rows', 10000],
  ['eval.array.map.pluck', 10000],
  ['eval.array.sort', 10000],
  ['eval.array.push-reduce', 10000],
  ['eval.object.groupBy.pluck', 10000],
  ['eval.object.spread-reduce', 1000],
  ['eval.string.join', 10000],
  ['eval.string.replaceAll', 10000],
  ['eval.string.padStart', 10000],
  ['eval.string.encodeURIComponent', 10000],
  ['eval.regex.test', 10000],
  ['eval.regex.test.many', 1000],
  ['eval.regex.replace', 10000],
  ['eval.regex.split', 10000],
  ['eval.set.has', 10000],
  ['eval.set.union', 10000],
  ['eval.map.get', 10000],
  ['eval.map.groupBy', 10000],
  ['eval.bigint.factorial', 10000],
  ['eval.json.parse', 10000],
  ['eval.json.stringify', 10000],
  ['eval.number.parse', 10000],
].map(([name, n]) => ({ name, n: n ?? null }));
