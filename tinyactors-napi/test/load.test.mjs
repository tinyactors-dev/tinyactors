// The addon loads in the main thread and in a worker: its state is the
// environment's instance data (docs/decisions.md, "Per-environment state").
import { Worker } from 'node:worker_threads';
import { test, run } from './harness.mjs';
import addon from '@tinyactors/node';

test('the addon loads', () => {
  if (typeof addon !== 'object' || addon === null) throw new Error('no addon');
});

test('the addon loads in a worker beside the main thread', async () => {
  const code = "import('@tinyactors/node').then((m) => require('node:worker_threads').parentPort.postMessage(typeof m.default))";
  const worker = new Worker(code, { eval: true });
  const kind = await new Promise((resolve, reject) => {
    worker.once('message', resolve);
    worker.once('error', reject);
  });
  await worker.terminate();
  if (kind !== 'object') throw new Error(`the worker saw ${kind}`);
});

await run();
