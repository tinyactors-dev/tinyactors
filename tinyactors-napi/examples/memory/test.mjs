// The measurements run, and each core column is what the core's own
// definitions give: the reservation is ta-layout's region for the same
// image and capacities (tinyactors-core's tools/layout/layout.c, which
// `mise run build` builds beside the addon as build/ta-layout), the record
// sizes are ta-layout's own check, and the use is those records times the
// pools' counts. The process column is the machine's and the engine's,
// never asserted. Run by `mise run examples memory`.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { compiled, memory, run, sizes } from './main.mjs';

const COUNT = 5000;
const LAYOUT = fileURLToPath(new URL('../../build/ta-layout', import.meta.url));

/** What ta-layout prints for `args`, as a map of name -> bytes. */
function layout(...args) {
  const result = spawnSync(LAYOUT, args.map(String), { encoding: 'utf8' });
  assert.equal(result.status, 0, `ta-layout ${args.join(' ')}: ${result.stderr}`);
  return Object.fromEntries(result.stdout.trim().split('\n').map((line) => {
    const [name, bytes] = line.split(' ');
    return [name, Number(bytes)];
  }));
}

/** ta-layout's reservation per thing of a row: its region at COUNT and at
 * none, the same capacities the example's hosts are given. */
function reservation(image, what, tracing) {
  const region = (n) => layout(image, ...Object.entries(sizes(what, COUNT, n, tracing)).map(([name, value]) => `${name}=${value}`)).region;
  return Math.round((region(COUNT) - region(0)) / COUNT);
}

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
    test(`${datamodel}, tracing ${tracing ? 'on' : 'off'}: each core column is what the core's definitions give`, async () => {
      const directory = mkdtempSync(join(tmpdir(), 'tinyactors-memory-'));
      try {
        const image = join(directory, 'counter.tai');
        writeFileSync(image, compiled().image);
        await check(image, datamodel, tracing);
      } finally {
        rmSync(directory, { recursive: true, force: true });
      }
    });
  }
}

async function check(image, datamodel, tracing) {
  const results = await run({ count: COUNT, datamodel, tracing });
  // the records: what core checks against its types' debug information
  const records = layout();
  assert.deepEqual(results.units, { actor: records.actor, event: records[tracing ? 'event' : 'untraced-event'], timer: records.timer });
  const { units } = results;
  const rows = {
    started: [results.actor.started, 'actor'], handled: [results.actor.handled, 'actor'],
    hibernated: [results.hibernated.hibernated, 'hibernated'], waiting: [results.message.waiting, 'message'],
    pending: [results['delayed send'].pending, 'delayed send'],
  };
  for (const [key, [row, what]] of Object.entries(rows)) {
    assert.equal(row.failed, undefined, `${key}: ${row.failed}`);
    // the reservation: ta-layout's region for the same image and capacities
    assert.equal(row.reserved, reservation(image, what, tracing), `${key}: core reserved`);
    // every pool the row fills was sized for it: none grew
    assert.deepEqual(row.grown, [], `${key}: pools that grew`);
    if (datamodel === 'native') assert.equal(row.quickjs, undefined, `${key}: quickjs on a native host`);
    else assert.ok(row.quickjs > 0, `${key}: quickjs ${row.quickjs} B`);
    assert.equal(typeof row.process, 'number', `${key}: process`); // the machine's: never asserted
  }
  const { started, hibernated, waiting, pending } = Object.fromEntries(Object.entries(rows).map(([key, [row]]) => [key, row]));
  // core used: the pools' records in use, and the slab
  assert.deepEqual(waiting.pools, { actors: 0, events: COUNT, timers: 0 });
  assert.equal(waiting.used, units.event, 'a message waiting uses its event');
  assert.equal(waiting.reserved, units.event, 'a message reserves its event');
  // quickjs keeps the event an actor handled last, as its _event, in the
  // core's pool until the next one (the native datamodel detaches it): one
  // more event on quickjs for the delayed sends' actor, one per actor after
  // an event
  const kept = datamodel === 'quickjs' ? 1 : 0;
  assert.deepEqual(pending.pools, { actors: 0, events: COUNT + kept, timers: COUNT });
  assert.equal(pending.used, units.timer + units.event, 'a delayed send uses its timer and its message');
  assert.equal(pending.reserved, units.timer + units.event, 'a delayed send reserves its timer and its message');
  assert.equal(started.pools.actors, COUNT);
  assert.deepEqual(rows.handled[0].pools, { actors: COUNT, events: kept * COUNT, timers: 0 });
  assert.ok(started.used > units.actor, `a started actor uses its record and its instance: ${started.used} B`);
  assert.equal(hibernated.pools.actors, COUNT);
  assert.ok(hibernated.used >= units.actor, `a hibernated actor keeps its record: ${hibernated.used} B`);
  assert.ok(hibernated.blob > 0, `a hibernated actor's blob: ${hibernated.blob} B`);
  // a host sized up front allocates nothing per thing beside its region
  for (const row of [started, waiting, pending]) assert.equal(row.hostOther, 0, JSON.stringify(row));
}
