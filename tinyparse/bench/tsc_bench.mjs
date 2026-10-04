// tsc_bench.mjs -- parse-only benchmark of the TypeScript compiler's parser
// (the canonical hand-written recursive-descent TS parser) over the React
// corpus, the exact same byte set ci/pocketparse-react-bench.sh uses.
//
// We call ts.createSourceFile(name, text, ScriptTarget.Latest,
// /*setParentNodes*/ false, ScriptKind.TSX) -- parse only. No Program is
// created, so no type-checker, no module resolution, no binder beyond what the
// parser itself does. This isolates parse time, matching the tsgo and
// pocketparse "parse-to-discard" measurements.
//
// Usage:  node tsc_bench.mjs MANIFEST ITERS
// Reads typescript from ./tsbench/node_modules (installed by the orchestrator).
//
// Output (one line, the field=value shape the bench scripts already parse):
//   files=.. bytes=.. top_statements=.. iterations=.. median_seconds=.. throughput_mbps=..

import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
// typescript lives in the gitignored scratch install next to this script.
const require = createRequire(join(here, "tsbench", "package.json"));
const ts = require("typescript");

const [, , manifestPath, itersArg] = process.argv;
if (!manifestPath || !itersArg) {
  console.error("usage: node tsc_bench.mjs MANIFEST ITERS");
  process.exit(2);
}
const iters = Number.parseInt(itersArg, 10);
if (!Number.isInteger(iters) || iters < 1) {
  console.error("ITERS must be a positive integer");
  process.exit(2);
}

const paths = readFileSync(manifestPath, "utf8")
  .split("\n")
  .map((s) => s.trim())
  .filter((s) => s.length > 0);

let totalBytes = 0;
const texts = paths.map((p) => {
  const buf = readFileSync(p);
  totalBytes += buf.length; // byte count, matching the corpus size figure
  return buf.toString("utf8");
});

function parseAll() {
  let topStatements = 0;
  for (let i = 0; i < texts.length; i++) {
    // Parse only: no parent links, TSX script kind, latest target.
    const sf = ts.createSourceFile(
      paths[i],
      texts[i],
      ts.ScriptTarget.Latest,
      /*setParentNodes*/ false,
      ts.ScriptKind.TSX,
    );
    topStatements += sf.statements.length;
  }
  return topStatements;
}

// Warm up (JIT, lazy module init).
const warm = parseAll();
parseAll();

const secs = [];
for (let it = 0; it < iters; it++) {
  const start = process.hrtime.bigint();
  parseAll();
  const end = process.hrtime.bigint();
  secs.push(Number(end - start) / 1e9);
}
secs.sort((a, b) => a - b);
const median = secs[Math.floor(secs.length / 2)];
const mb = totalBytes / (1024 * 1024);
const mbps = mb / median;

process.stdout.write(
  `files=${paths.length} bytes=${totalBytes} top_statements=${warm} ` +
    `iterations=${iters} median_seconds=${median.toFixed(6)} ` +
    `throughput_mbps=${mbps.toFixed(2)} ts_version=${ts.version}\n`,
);
