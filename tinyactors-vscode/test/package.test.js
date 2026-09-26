'use strict';

// The .vsix `mise run build` writes (mise/tasks/build), read back with a
// zip reader of its own: it holds the extension's files as they are, and
// its manifest names the extension package.json describes.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { test } = require('node:test');

const ROOT = path.resolve(__dirname, '..');
const VSIX = path.join(ROOT, 'build', 'tinyactors-vscode.vsix');

// name -> contents, from the central directory (APPNOTE.TXT 4.3.12).
function unzip(archive) {
  const end = archive.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  assert.ok(end >= 0, 'the archive has an end of central directory');
  const count = archive.readUInt16LE(end + 10);
  const files = new Map();
  let at = archive.readUInt32LE(end + 16);
  for (let index = 0; index < count; index += 1) {
    assert.equal(archive.readUInt32LE(at), 0x02014b50, 'a central directory entry');
    const method = archive.readUInt16LE(at + 10);
    const packed = archive.readUInt32LE(at + 20);
    const size = archive.readUInt32LE(at + 24);
    const length = archive.readUInt16LE(at + 28);
    const skip = length + archive.readUInt16LE(at + 30) + archive.readUInt16LE(at + 32);
    const local = archive.readUInt32LE(at + 42);
    const name = archive.toString('utf8', at + 46, at + 46 + length);
    const start = local + 30 + archive.readUInt16LE(local + 26) + archive.readUInt16LE(local + 28);
    const data = archive.subarray(start, start + packed);
    const contents = method === 8 ? zlib.inflateRawSync(data) : data;
    assert.equal(contents.length, size, `${name} has its size`);
    assert.equal(zlib.crc32(contents), archive.readUInt32LE(at + 16), `${name} has its CRC`);
    files.set(name, contents);
    at += 46 + skip;
  }
  return files;
}

test('the .vsix holds the extension and a manifest that names it', () => {
  const files = unzip(fs.readFileSync(VSIX));
  const sources = ['package.json', 'README.md', ...fs.readdirSync(path.join(ROOT, 'lib')).map((file) => `lib/${file}`)];
  assert.deepEqual([...files.keys()].sort(),
    ['[Content_Types].xml', 'extension.vsixmanifest', ...sources.map((file) => `extension/${file}`)].sort());
  for (const file of sources) {
    assert.ok(files.get(`extension/${file}`).equals(fs.readFileSync(path.join(ROOT, file))), `${file} is packed as it is`);
  }
  const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));
  const vsixManifest = files.get('extension.vsixmanifest').toString('utf8');
  assert.match(vsixManifest, new RegExp(`<Identity Language="en-US" Id="${manifest.name}" Version="${manifest.version}" `
    + `Publisher="${manifest.publisher}" />`));
  assert.ok(vsixManifest.includes(`Id="Microsoft.VisualStudio.Code.Engine" Value="${manifest.engines.vscode}"`));
});
