// meriyah_bench.mjs -- parse-only benchmark of meriyah (the fastest JS-in-JS
// parser) over the React corpus, the exact same byte set the other bench arms
// use. Meriyah is the OWNER'S BAR: "if we can't get close to meriyah we're not
// in a good spot," so every bench run reports it.
//
// Meriyah parses ECMAScript + JSX, NOT TypeScript. The React corpus is the file
// set std.tsx parses cleanly; most are plain JSX modules meriyah accepts, but a
// few carry Flow/TS syntax meriyah rejects. We parse every file with
// meriyah.parseModule(source, { jsx: true, ... }); a file meriyah cannot parse
// is COUNTED and its bytes are EXCLUDED from the throughput denominator, and the
// failure count is reported, so the meriyah MB/s figure is over the bytes meriyah
// actually parsed (an honest like-for-like on the shared-parseable subset, with
// the coverage stated alongside).
//
// Usage:  node meriyah_bench.mjs MANIFEST ITERS
// Reads meriyah from ./tsbench/node_modules (installed by the orchestrator).
//
// Output (one line, the field=value shape the bench scripts already parse):
//   files=.. parsed=.. failed=.. bytes=.. iterations=.. median_seconds=.. throughput_mbps=.. meriyah_version=..

import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
// meriyah lives in the gitignored scratch install next to this script.
const require = createRequire(join(here, "tsbench", "package.json"));
const meriyah = require("meriyah");
let meriyahVersion = "unknown";
try {
  // meriyah's "exports" map blocks the package.json subpath, so read it directly.
  meriyahVersion = JSON.parse(
    readFileSync(
      join(here, "tsbench", "node_modules", "meriyah", "package.json"),
      "utf8",
    ),
  ).version;
} catch {
  /* version is cosmetic; leave it "unknown" */
}

const [, , manifestPath, itersArg] = process.argv;
if (!manifestPath || !itersArg) {
  console.error("usage: node meriyah_bench.mjs MANIFEST ITERS");
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

const opts = { jsx: true, module: true, next: true, webcompat: true };

// Determine which files meriyah can parse, and the byte total over those files.
// A file meriyah rejects is excluded from the timed work and the denominator.
const parseable = [];
let parsedBytes = 0;
let failed = 0;
for (let i = 0; i < paths.length; i++) {
  const buf = readFileSync(paths[i]);
  const text = buf.toString("utf8");
  try {
    meriyah.parseModule(text, opts);
    parseable.push(text);
    parsedBytes += buf.length;
  } catch {
    failed++;
  }
}

function parseAll() {
  let nodes = 0;
  for (let i = 0; i < parseable.length; i++) {
    const ast = meriyah.parseModule(parseable[i], opts);
    nodes += ast.body.length;
  }
  return nodes;
}

// Warm up (JIT).
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
const mb = parsedBytes / (1024 * 1024);
const mbps = mb / median;

process.stdout.write(
  `files=${paths.length} parsed=${parseable.length} failed=${failed} ` +
    `bytes=${parsedBytes} top_statements=${warm} iterations=${iters} ` +
    `median_seconds=${median.toFixed(6)} throughput_mbps=${mbps.toFixed(2)} ` +
    `meriyah_version=${meriyahVersion}\n`,
);
