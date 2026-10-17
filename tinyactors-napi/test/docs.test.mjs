// The documentation's examples run as they are written (docs/plan.xml,
// p2-docs): every ```js block of README.md and docs/guide/*.md is a
// complete program, run in a process of its own by the runtime running
// this file (node or bun), from a directory inside the package so that
// `import ... from '@tinyactors/node'` resolves to this build. A block
// followed by an ```output block must print exactly that. A document
// that drifts from the API fails here. The blocks are read by
// tools/examples.mjs, which tools/typecheck.mjs shares.
//
// The examples name their own datamodel, so they run in the native pass
// of the test task only (TA_NAPI_DATAMODEL), once per runtime.
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { documents, examples as readExamples, fileName } from '../tools/examples.mjs';
import { DATAMODEL } from './charts.mjs';
import { run, test } from './harness.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const DOCUMENTS = documents(ROOT);
const examples = readExamples(ROOT);

/** Runs `file` with this runtime: its exit code, stdout and stderr. */
function execute(file, cwd) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [file], { cwd, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => (stdout += chunk));
    child.stderr.on('data', (chunk) => (stderr += chunk));
    const timer = setTimeout(() => child.kill('SIGKILL'), 60_000);
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal, stdout, stderr });
    });
  });
}

test('the documents have examples', () => {
  if (examples.length < DOCUMENTS.length) throw new Error(`${examples.length} examples in ${DOCUMENTS.length} documents`);
  for (const document of DOCUMENTS)
    if (!examples.some((example) => example.document === document)) throw new Error(`${document} has no example`);
});

test('every export of index.d.ts, and every member of Host, has a doc comment', () => {
  const lines = readFileSync(join(ROOT, 'index.d.ts'), 'utf8').split('\n');
  const missing = [];
  let inHost = false;
  lines.forEach((line, i) => {
    if (/^export class Host\b/.test(line)) inHost = true;
    else if (inHost && line === '}') inHost = false;
    const declares = /^export /.test(line) || (inHost && /^ {2}[^ /*].*;$/.test(line));
    if (!declares) return;
    let j = i - 1;
    while (j >= 0 && lines[j].trim() === '') j--;
    if (!lines[j]?.trim().endsWith('*/')) missing.push(`${i + 1}: ${line.trim()}`);
  });
  if (missing.length) throw new Error(`no doc comment:\n${missing.join('\n')}`);
});

if (DATAMODEL === 'native') {
  const directory = mkdtempSync(join(ROOT, 'build', 'docs-'));
  // a few at a time: each is a runtime of its own
  const pending = examples.map(() => undefined);
  let next = 0;
  const start = () => {
    if (next === examples.length) return;
    const i = next++;
    const file = join(directory, fileName(examples[i]));
    writeFileSync(file, examples[i].program);
    pending[i].resolve(execute(file, directory).finally(start));
  };
  for (let i = 0; i < examples.length; i++) {
    let resolve;
    const result = new Promise((r) => (resolve = r));
    pending[i] = { result, resolve };
  }
  for (let i = 0; i < 4; i++) start();

  examples.forEach((example, i) => {
    test(`${example.document}:${example.line} runs as written`, async () => {
      const { code, signal, stdout, stderr } = await pending[i].result;
      if (code !== 0) throw new Error(`exit ${code ?? signal}\n${stderr}${stdout}`);
      if (example.output !== undefined && stdout.trimEnd() !== example.output.trimEnd())
        throw new Error(`printed:\n${stdout}\nexpected:\n${example.output}\n${stderr}`);
    });
  });
  test('the examples\' directory is removed', () => rmSync(directory, { recursive: true, force: true }));
}

await run();
