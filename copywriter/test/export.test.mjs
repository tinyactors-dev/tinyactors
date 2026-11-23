import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setup, fakeFetch, geminiReply, find } from './fakes.mjs';
import { unzip, zip, crc32 } from '../src/core/packet.mjs';

const projectDir = new URL('../', import.meta.url).pathname;
const decoder = new TextDecoder();

// sxml, as mise pins it for this project.
function sxml(...args) {
  const bin = execFileSync('mise', ['which', 'sxml'], { cwd: projectDir, encoding: 'utf8' }).trim();
  return execFileSync(bin, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
}

async function reviewed() {
  const fetch = fakeFetch(geminiReply('One plainer line.', [
    ['propose_rewrite', { replacement: 'Ship reliable software', rationale: 'Names the outcome & the reader.' }],
  ]));
  const env = await setup({ fetch, text: 'Build better products. Actors are <cheap>. Read the guide.' });
  const { reviewer, page } = env;
  page.select('Build better products');
  const kept = await reviewer.dispatch({ type: 'annotate', note: 'Too generic.' });
  await reviewer.dispatch({ type: 'preview', id: kept, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id: kept });
  page.select('Actors are <cheap>');
  const open = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'note', id: open, text: 'Cheap how? Give a number "in bytes".' });
  page.select('Read the guide');
  const dismissed = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'note', id: dismissed, text: 'Fine as it is.' });
  await reviewer.dispatch({ type: 'dismiss', id: dismissed });
  return { ...env, kept, open, dismissed };
}

test('export: a ZIP of packet.xml (valid), CHANGES.md, README.md and the schema', async () => {
  const { reviewer, downloads } = await reviewed();
  await reviewer.dispatch({ type: 'export', scope: 'page', to: 'download' });
  assert.equal(reviewer.getState().exporting, 'done');
  assert.equal(downloads.length, 1);
  assert.match(downloads[0].name, /^copywriter-packet-\d{8}-\d{4}\.zip$/);
  const files = Object.fromEntries(unzip(downloads[0].bytes).map((f) => [f.name, decoder.decode(f.data)]));
  assert.deepEqual(Object.keys(files), ['packet.xml', 'CHANGES.md', 'README.md', 'work-packet.xsd']);

  const dir = mkdtempSync(join(tmpdir(), 'copywriter-'));
  try {
    for (const [name, text] of Object.entries(files)) writeFileSync(join(dir, name), text);
    sxml('validate', `--schema=${join(dir, 'work-packet.xsd')}`, join(dir, 'packet.xml'));
    const items = sxml('xpath', `--context-item=${join(dir, 'packet.xml')}`,
      '/work-packet/payload/items/*/concat(local-name(), ": ", (replacement, summary)[1])').trim().split('\n');
    assert.deepEqual(items, ['"copy-change: Ship reliable software"', '"design-task: Cheap how? Give a number ""in bytes""."']);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }

  const changes = files['CHANGES.md'];
  assert.match(changes, /^# Copy changes/);
  assert.match(changes, /## https:\/\/www\.tinyactors\.dev\/ \(tinyactors\)/);
  assert.match(changes, /Original:\n\n```text\nBuild better products\n```\n\nReplacement:\n\n```text\nShip reliable software\n```/);
  assert.match(changes, /Why: Names the outcome & the reader\./);
  assert.match(changes, /Reviewer's note: Too generic\./);
  assert.match(changes, /- element: `main > p`/);
  assert.match(changes, /### Open note 2[\s\S]*Actors are <cheap>[\s\S]*Note: Cheap how\?/);
  assert.doesNotMatch(changes, /Fine as it is/, 'a dismissed annotation is not exported');
  assert.match(files['README.md'], /1 kept change\(s\) and 1 open note\(s\) on 1 page\(s\)/);
  reviewer.close();
});

test('export to the clipboard: CHANGES.md', async () => {
  const { reviewer, clipboard } = await reviewed();
  await reviewer.dispatch({ type: 'export', scope: 'all', to: 'clipboard' });
  assert.equal(clipboard.length, 1);
  assert.match(clipboard[0], /Replacement:\n\n```text\nShip reliable software/);
  reviewer.close();
});

test('export with nothing kept and no note is refused', async () => {
  const { reviewer, downloads } = await setup();
  await assert.rejects(reviewer.dispatch({ type: 'export', scope: 'page', to: 'download' }), /Nothing to export/);
  assert.equal(reviewer.getState().exporting, 'failed');
  assert.equal(downloads.length, 0);
  reviewer.close();
});

test('the key is never in the store, the export or the charts\' sends', async () => {
  const { reviewer, store, downloads, clipboard, fetch } = await reviewed();
  await reviewer.dispatch({ type: 'settings.save', model: 'gemini-4-carbon' });
  await reviewer.dispatch({ type: 'export', scope: 'all', to: 'download' });
  await reviewer.dispatch({ type: 'export', scope: 'all', to: 'clipboard' });
  const key = 'AIza-test-key-123';
  assert.equal(fetch.requests[0].headers['x-goog-api-key'], key, 'the key did reach Gemini');
  assert.ok(!store.dump().includes(key));
  for (const f of unzip(downloads[0].bytes)) assert.ok(!decoder.decode(f.data).includes(key), f.name);
  assert.ok(!clipboard.join('').includes(key));
  assert.ok(!JSON.stringify(reviewer.getState()).includes(key));
  for (const r of fetch.requests) assert.ok(!JSON.stringify(r.body).includes(key));
  reviewer.close();
});

test('zip: stored entries with their CRC-32, read back', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  const bytes = zip([{ name: 'a.txt', data: 'hello' }, { name: 'dir/ü.bin', data: new Uint8Array([0, 1, 2]) }]);
  const files = unzip(bytes);
  assert.deepEqual(files.map((f) => f.name), ['a.txt', 'dir/ü.bin']);
  assert.equal(decoder.decode(files[0].data), 'hello');
  assert.deepEqual([...files[1].data], [0, 1, 2]);
  assert.throws(() => zip([{ name: '../x', data: '' }]), /Unsafe/);
});
