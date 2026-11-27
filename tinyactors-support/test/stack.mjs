// The whole service in one process, on a SimulatedClock, with the
// stand-ins: what the tests drive. Each call to stack() is its own.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { crc32 } from 'node:zlib';
import { SimulatedClock } from '@tinyactors/node';
import { Support } from '../lib/host.mjs';
import { Gateway } from '../lib/gateway.mjs';
import { Content } from '../lib/content.mjs';
import { MemoryBlobs } from '../lib/processors/blobs.mjs';
import { Ledger } from '../lib/processors/ledger.mjs';
import { Scanner } from '../lib/processors/scan.mjs';
import { FakeGitHub, FakeJev } from '../lib/processors/standins.mjs';

export const BASE = 'https://support.test';
const CLI = new URL('../../tinyactors-core/build/dev/bin/tinyactors', import.meta.url).pathname;

export function sha(bytes) { return createHash('sha256').update(bytes).digest('hex'); }

export async function stack({ data = mkdtempSync(join(tmpdir(), 'support-')), resident = 6, shared = null } = {}) {
  const parts = shared ?? {
    github: new FakeGitHub(), jev: new FakeJev(), ledger: new Ledger(),
    blobs: new MemoryBlobs({ secret: 'test', base: BASE }),
  };
  parts.scanner ??= new Scanner(parts.blobs, existsSync(CLI) ? CLI : undefined);
  const clock = new SimulatedClock(), ended = [], faults = [];
  const support = new Support({
    data, clock, resident, snapshotDelay: 1e9,
    log: (line) => { if (/faulted|failed/.test(line)) faults.push(line); },
    processors: { github: parts.github.processor, ledger: parts.ledger.processor, decide: parts.jev.processor,
                  blobs: parts.blobs.processor, scan: parts.scanner.processor },
  });
  support.onDone = (_actor, donedata) => ended.push(donedata);
  const content = new Content({ skills: new URL('../skills/', import.meta.url).pathname,
                                docs: [{ directory: new URL('./fixtures/docs/', import.meta.url).pathname, prefix: 'docs' }] });
  const gateway = new Gateway({ support, base: BASE, blobs: parts.blobs, content });
  const idle = () => support.host.idle();
  await idle();

  async function advance(ms) { clock.increment(ms); await idle(); }

  // A tool call as the MCP layer makes it: the caller from the token.
  async function tool(token, name, args = {}) {
    const caller = token ? await gateway.caller(token) : null;
    const answer = await gateway.callTool(name, args, caller);
    await idle();
    return answer.result ?? answer;
  }

  // An MCP client signing a person in: PKCE, the stand-in GitHub, the code for tokens.
  async function signIn(githubCode) {
    const verifier = `verifier-for-${githubCode}-0123456789abcdefghijklmnopqrstuvwxyz`;
    const client = { clientID: 'claude-ai', redirectURI: 'https://claude.ai/api/mcp/auth_callback', clientState: 'xyz' };
    const started = await gateway.authorize({ ...client, challenge: createHash('sha256').update(verifier).digest('base64url'),
                                              resource: `${BASE}/mcp` });
    assert.equal(started.status, 302);
    const state = new URL(started.location).searchParams.get('state');
    const back = await gateway.githubCallback({ state, code: githubCode });
    assert.equal(back.status, 302, JSON.stringify(back));
    const tokens = await gateway.token({ code: back.code, verifier, redirectURI: client.redirectURI });
    assert.equal(tokens.status, 200, JSON.stringify(tokens));
    await idle();
    return tokens;
  }

  // An agent attaching a file: the slot by MCP, then the bytes by PUT, as
  // curl -T sends them. `lie` declares another size or sha256.
  async function attach(token, id, name, type, bytes, lie = {}) {
    const slot = await tool(token, 'attach_file', { id, name, type, size: lie.size ?? bytes.length, sha256: lie.sha256 ?? sha(bytes) });
    if (slot.answer !== 'upload.opened') return { slot };
    const put = await gateway.upload({ token, uploadID: slot.uploadID, chunks: [bytes.subarray(0, 8), bytes.subarray(8)] });
    await idle();
    return { slot, put };
  }

  return { ...parts, clock, support, gateway, content, ended, faults, data, idle, advance, tool, signIn, attach };
}

// A zip of stored entries (local headers only, which is what the scan
// reads); `claim` writes another uncompressed size into the headers.
export function zip(entries, { claim } = {}) {
  return Buffer.concat(entries.map(([name, text]) => {
    const data = Buffer.from(text), header = Buffer.alloc(30), file = Buffer.from(name);
    header.writeUInt32LE(0x04034b50, 0); header.writeUInt16LE(20, 4);
    header.writeUInt32LE(crc32(data), 14); header.writeUInt32LE(data.length, 18);
    header.writeUInt32LE(claim ?? data.length, 22); header.writeUInt16LE(file.length, 26);
    return Buffer.concat([header, file, data]);
  }));
}
