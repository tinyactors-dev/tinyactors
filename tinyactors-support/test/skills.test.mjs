// The MCP Skills extension (io.modelcontextprotocol/skills, Beta), as a
// host sees it over HTTP: the capability in initialize, skills/list,
// skills/get and resources/directory/read, each result valid against the
// extension's own JSON Schema (test/fixtures/ext-skills-beta-schema.json:
// modelcontextprotocol/ext-skills, schema/beta/schema.json at ba23984a,
// 2026-11-27), and the rules a schema cannot check, which hosts verify:
// every listed file's digest and size match the bytes resources/read
// gives; the frontmatter is SKILL.md's; the last segment of the skill's
// path is its name; a skill is never split across pages; -32602 for what
// is not a skill or not a directory. The tools list_skills and get_skill
// give the same skills, URIs and digests.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { after, before, describe, it } from 'node:test';
import Ajv2020 from 'ajv/dist/2020.js';
import { serve } from '../lib/http.mjs';
import { frontmatterOf } from '../lib/skills.mjs';
import { stack } from './stack.mjs';

const SCHEMA = JSON.parse(readFileSync(new URL('./fixtures/ext-skills-beta-schema.json', import.meta.url), 'utf8'));
const ajv = new Ajv2020({ strict: false, allErrors: true });
ajv.addSchema(SCHEMA);
const valid = (name, value) => {
  const check = ajv.getSchema(`${SCHEMA.$id}#/$defs/${name}`);
  assert.ok(check(value), `${name}: ${ajv.errorsText(check.errors)}`);
};
// The revision 2026-07-28's request metadata, which a client of that revision sends.
const META = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28',
               'io.modelcontextprotocol/clientInfo': { name: 'test', version: '1' },
               'io.modelcontextprotocol/clientCapabilities': {} };

describe('the Skills extension', () => {
  let s, server, base;
  before(async () => {
    s = await stack();
    // one skill a page, so paging and the atomicity of an entry show
    server = await serve({ support: s.support, gateway: s.gateway, pageSize: 1 });
    base = `http://127.0.0.1:${server.address().port}`;
    s.gateway.base = base;
  });
  after(async () => { server.close(); await s.support.close(); });

  let id = 0;
  async function rpc(method, params) {
    const response = await fetch(`${base}/mcp`, { method: 'POST',
      headers: { 'content-type': 'application/json', accept: 'application/json, text/event-stream' },
      body: JSON.stringify({ jsonrpc: '2.0', id: ++id, method, params }) });
    return JSON.parse(await response.text());
  }
  // every skill, page by page, each page valid
  async function listAll(params = {}) {
    const skills = [];
    let cursor;
    do {
      const { result } = await rpc('skills/list', { ...params, ...(cursor ? { cursor } : {}) });
      valid('ListSkillsResult', result);
      assert.equal(result.resultType, 'complete');
      assert.equal(result.cacheScope, 'public');
      assert.ok(result.ttlMs > 0);
      assert.ok(result.skills.length <= 1, 'one skill a page');
      skills.push(...result.skills);
      cursor = result.nextCursor;
    } while (cursor);
    return skills;
  }

  it('is declared in initialize\'s capabilities, with resources and directoryRead', async () => {
    const { result } = await rpc('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'test', version: '1' } });
    assert.deepEqual(result.capabilities.extensions, { 'io.modelcontextprotocol/skills': { directoryRead: true } });
    assert.ok(result.capabilities.resources, 'a server declaring the extension declares resources');
  });

  it('lists every skill as a complete manifest, a page at a time, never splitting one', async () => {
    const skills = await listAll();
    assert.deepEqual(skills.map((k) => k.uri), ['skill://reporting-tinyactors-issues/SKILL.md', 'skill://using-tinyactors/SKILL.md']);
    assert.deepEqual(await listAll({ _meta: META }), skills, 'a 2026-07-28 request with its _meta gets the same');
    for (const skill of skills) {
      valid('Skill', skill);
      const name = skill.uri.replace(/^skill:\/\//, '').replace(/\/SKILL\.md$/, '').split('/').pop();
      assert.equal(name, skill.frontmatter.name, 'the last segment of the skill\'s path is its name');
      assert.ok(Array.isArray(skill.resources), 'a static skill publishes its files');
      assert.equal(skill.resources[0].uri, skill.uri, 'the manifest lists SKILL.md itself');
      assert.equal(new Set(skill.resources.map((r) => r.uri)).size, skill.resources.length, 'each file once');
      assert.ok(skill.resources.length <= 512 && skill.resources.reduce((n, r) => n + r.size, 0) <= 16 * 1024 * 1024);
      for (const file of skill.resources) {
        assert.ok(file.uri.startsWith(skill.uri.replace(/SKILL\.md$/, '')), `${file.uri} is in the skill`);
        assert.match(file.digest, /^sha256:[0-9a-f]{64}$/);
        // the bytes a host reads are the bytes the manifest describes
        const { result } = await rpc('resources/read', { uri: file.uri });
        const content = result.contents[0];
        const bytes = content.blob !== undefined ? Buffer.from(content.blob, 'base64') : Buffer.from(content.text, 'utf8');
        assert.equal(bytes.length, file.size, `${file.uri}: size`);
        assert.equal(`sha256:${createHash('sha256').update(bytes).digest('hex')}`, file.digest, `${file.uri}: digest`);
        if (file.uri === skill.uri) {
          assert.equal(content.mimeType, 'text/markdown');
          assert.deepEqual(frontmatterOf(content.text, file.uri), skill.frontmatter, 'the frontmatter is SKILL.md\'s, field by field');
        }
      }
    }
  });

  it('gets one skill by its SKILL.md\'s URI, and refuses what is not a skill with -32602', async () => {
    const [first] = await listAll();
    const { result } = await rpc('skills/get', { uri: first.uri, _meta: META });
    valid('GetSkillResult', result);
    assert.equal(result.resultType, 'complete');
    assert.equal(result.cacheScope, 'public');
    assert.deepEqual(result.skill, first, 'the same entry as the listing');
    for (const uri of ['skill://nope/SKILL.md', 'skill://using-tinyactors', 'skill://using-tinyactors/references/pitfalls.md'])
      assert.equal((await rpc('skills/get', { uri })).error.code, -32602, uri);
  });

  it('reads a directory\'s direct children, a page at a time, and refuses a file or an unknown path with -32602', async () => {
    const children = [];
    let cursor;
    do {
      const { result } = await rpc('resources/directory/read', { uri: 'skill://using-tinyactors', ...(cursor ? { cursor } : {}) });
      valid('ReadResourceDirectoryResult', result);
      children.push(...result.resources);
      cursor = result.nextCursor;
    } while (cursor);
    assert.deepEqual(children.map((c) => [c.uri, c.mimeType]),
      [['skill://using-tinyactors/SKILL.md', 'text/markdown'], ['skill://using-tinyactors/references', 'inode/directory']]);
    assert.equal(children[0].name, 'using-tinyactors', 'SKILL.md is named by its frontmatter');
    const references = [];
    do {
      const { result } = await rpc('resources/directory/read', { uri: 'skill://using-tinyactors/references', ...(cursor ? { cursor } : {}) });
      valid('ReadResourceDirectoryResult', result);
      references.push(...result.resources);
      cursor = result.nextCursor;
    } while (cursor);
    const skill = (await listAll()).find((k) => k.frontmatter.name === 'using-tinyactors');
    assert.deepEqual(references.map((r) => r.uri).sort(),
      skill.resources.map((r) => r.uri).filter((u) => u.includes('/references/')).sort(), 'the directory and the manifest agree');
    for (const uri of ['skill://using-tinyactors/SKILL.md', 'skill://nope', 'skill://using-tinyactors/references/'])
      assert.equal((await rpc('resources/directory/read', { uri })).error.code, -32602, uri);
    assert.equal((await rpc('resources/directory/read', { uri: 'skill://using-tinyactors', cursor: 'nope' })).error.code, -32602);
  });

  it('serves the same skills through the tools, for hosts without the extension', async () => {
    const listed = await listAll();
    const tool = async (name, args) => (await rpc('tools/call', { name, arguments: args })).result.structuredContent;
    const { skills } = await tool('list_skills', {});
    assert.deepEqual(skills.map((k) => [k.name, k.description, k.uri]),
      listed.map((k) => [k.frontmatter.name, k.frontmatter.description, k.uri]));
    for (const skill of listed) {
      for (const file of skill.resources) {
        const path = file.uri.slice(skill.uri.length - 'SKILL.md'.length);
        const got = await tool('get_skill', { name: skill.frontmatter.name, ...(path === 'SKILL.md' ? {} : { path }) });
        assert.deepEqual([got.uri, got.digest, got.size], [file.uri, file.digest, file.size], `get_skill ${path}`);
      }
    }
  });

  it('answers resources/read of an unknown skill file as not found, not as a failure of the server', async () => {
    assert.equal((await rpc('resources/read', { uri: 'skill://using-tinyactors/references/nope.md' })).error.code, -32002);
  });
});
