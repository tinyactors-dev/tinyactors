// The measurements run, and the core's share is what `mise run layout` in
// tinyactors-core gives: a message 106 B (57 B without tracing), a delayed
// send 151 B (102 B). Run by `mise run examples memory`.
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { test } from 'node:test';
import { memory, run } from './main.mjs';

const MADV_PAGEOUT = 21; // Linux's <sys/mman.h>, since 5.4

/**
 * Ask the kernel to reclaim this process's pages of mapped files now, as it
 * does when the machine runs short of memory, with libc's madvise through
 * bun:ffi; the bytes it took, 0 where it cannot.
 */
async function pageOutMappedFiles() {
  const { dlopen } = await import('bun:ffi');
  const libc = dlopen('libc.so.6', { madvise: { args: ['u64', 'u64', 'i32'], returns: 'i32' } });
  let paged = 0n;
  for (const line of readFileSync('/proc/self/maps', 'utf8').split('\n')) {
    const fields = line.split(/\s+/);
    if (fields.length < 6 || !fields[5].startsWith('/')) continue;
    const [start, end] = fields[0].split('-').map((address) => BigInt(`0x${address}`));
    if (libc.symbols.madvise(start, end - start, MADV_PAGEOUT) === 0) paged += end - start;
  }
  libc.close();
  return paged;
}

// Under memory pressure the kernel drops the code of the runtime and the
// addon between two samples; counted, that made a row negative. Node has no
// FFI to call madvise from JavaScript: the run under bun checks memory(),
// which reads the same /proc file under both runtimes.
const pageOut = !existsSync('/proc/self/maps') ? 'Linux only: memory is the resident set elsewhere'
  : !globalThis.Bun ? 'node has no FFI for madvise; the run under bun checks it' : false;
test('memory leaves out the pages of mapped files the kernel reclaims', { skip: pageOut }, async (t) => {
  const before = memory();
  if (await pageOutMappedFiles() === 0n) return t.skip('the kernel refuses MADV_PAGEOUT');
  assert.ok(memory() >= before, `memory fell from ${before} B to ${memory()} B`);
});

for (const datamodel of ['native', 'quickjs']) {
  for (const tracing of [true, false]) {
    test(`${datamodel}, tracing ${tracing ? 'on' : 'off'}: each thing costs memory, and the core's share is its records`, () => {
      const results = run({ count: 5000, datamodel, tracing });
      assert.equal(results.message.waiting.region, tracing ? 106 : 57);
      assert.equal(results['delayed send'].pending.region, tracing ? 151 : 102);
      assert.ok(results.actor.started.region > 81, `an actor's region: ${results.actor.started.region} B`);
      // a hibernated actor keeps its records only: 81 B, up to 89 B as the indexes round up
      const { hibernated } = results.hibernated;
      assert.ok(hibernated.region >= 81 && hibernated.region <= 89, `a hibernated actor's region: ${hibernated.region} B`);
      assert.ok(hibernated.blob > 0, `a hibernated actor's blob: ${hibernated.blob} B`);
      for (const row of [results.actor.started, results.actor.handled, hibernated, results.message.waiting, results['delayed send'].pending]) {
        assert.ok(row.resident > 0, JSON.stringify(row));
      }
    });
  }
}
