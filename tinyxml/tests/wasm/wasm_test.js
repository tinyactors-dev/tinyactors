'use strict';

// txml.wasm as a JavaScript host uses it (mise run wasm-test, and the
// release's verify stage): the module is loaded with no imports, a document
// is placed in linear memory with txml_wasm_alloc(), parsed with the
// module's own allocator (txml_wasm_allocator()), checked, and freed. The
// structs are laid out from txml.api.json's wasm32 layouts, as a binding
// generator would. Parsing many times shows the allocator's free lists at
// work: after a warm-up, memory stops growing.
//
//   node tests/wasm/wasm_test.js <txml.wasm> <txml.api.json>

const assert = require('node:assert/strict');
const fs = require('node:fs');

const [wasmPath, apiPath] = process.argv.slice(2);
if (!wasmPath || !apiPath) {
  console.error('usage: node tests/wasm/wasm_test.js <txml.wasm> <txml.api.json>');
  process.exit(2);
}

const api = JSON.parse(fs.readFileSync(apiPath, 'utf8'));
const TARGET = 'wasm32-unknown-unknown';
const layout = (name) => api.types[name].layout[TARGET];
const constant = (type, name) => api.types[type].constants.find((entry) => entry.name === name).value;

const TXML_OK = constant('txml_result', 'TXML_OK');
const STEP = {
  progress: constant('txml_step', 'TXML_STEP_PROGRESS'),
  needInput: constant('txml_step', 'TXML_STEP_NEED_INPUT'),
  done: constant('txml_step', 'TXML_STEP_DONE'),
};
// TXML_LIMITS_DEFAULT, from txml.h.
const LIMITS = {
  max_depth: 256, max_attributes: 256, max_name_length: 1024, max_names: 4096,
  max_document_bytes: 64n * 1024n * 1024n, entity_expansion_budget: 8n * 1024n * 1024n,
};

async function main() {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {});
  const txml = instance.exports;
  const view = () => new DataView(txml.memory.buffer);
  const alloc = (size, align) => {
    const pointer = txml.txml_wasm_alloc(size, align);
    assert.notEqual(pointer, 0, `txml_wasm_alloc(${size}, ${align})`);
    new Uint8Array(txml.memory.buffer, pointer, size).fill(0);
    return pointer;
  };

  const version = txml.txml_version();
  const text = '<list><item>a</item><item>b</item><other/><item>c</item></list>';
  const input = new TextEncoder().encode(text);

  // One parse: the document in, a doc out, everything freed.
  function parse() {
    const allocatorLayout = layout('txml_allocator');
    const configLayout = layout('txml_parser_config');
    const limitsLayout = layout('txml_limits');
    const bytesLayout = layout('txml_bytes');

    // txml_wasm_allocator() returns its struct through a pointer (wasm32 C ABI).
    const allocator = alloc(allocatorLayout.size, allocatorLayout.align);
    txml.txml_wasm_allocator(allocator);
    const config = alloc(configLayout.size, configLayout.align);
    let memory = view();
    const offsets = configLayout.offsets;
    memory.setUint32(config + offsets.allocator + allocatorLayout.offsets.self,
      memory.getUint32(allocator + allocatorLayout.offsets.self, true), true);
    memory.setUint32(config + offsets.allocator + allocatorLayout.offsets.v,
      memory.getUint32(allocator + allocatorLayout.offsets.v, true), true);
    for (const [field, value] of Object.entries(LIMITS)) {
      const at = config + offsets.limits + limitsLayout.offsets[field];
      if (typeof value === 'bigint') memory.setBigUint64(at, value, true);
      else memory.setUint32(at, value, true);
    }

    const parserOut = alloc(4, 4);
    assert.equal(txml.txml_parser_init(parserOut, config), TXML_OK, 'txml_parser_init');
    const parser = view().getUint32(parserOut, true);

    const documentBytes = alloc(input.length, 1);
    new Uint8Array(txml.memory.buffer, documentBytes, input.length).set(input);
    const chunk = alloc(bytesLayout.size, bytesLayout.align);
    memory = view();
    memory.setUint32(chunk + bytesLayout.offsets.ptr, documentBytes, true);
    memory.setUint32(chunk + bytesLayout.offsets.length, input.length, true);

    let fed = false;
    for (;;) {
      const step = txml.txml_parse_step(parser);
      if (step === STEP.needInput && !fed) {
        txml.txml_parse_feed(parser, chunk, 1);
        fed = true;
      } else if (step === STEP.progress) {
        continue;
      } else {
        assert.equal(step, STEP.done, `txml_parse_step answered ${step}`);
        break;
      }
    }
    const docOut = alloc(4, 4);
    assert.equal(txml.txml_parser_finish(parser, docOut), TXML_OK, 'txml_parser_finish');
    const doc = view().getUint32(docOut, true);
    const nodes = txml.txml_doc_node_count(doc);
    txml.txml_doc_destroy(doc);

    txml.txml_wasm_free(docOut, 4, 4);
    txml.txml_wasm_free(chunk, bytesLayout.size, bytesLayout.align);
    txml.txml_wasm_free(documentBytes, input.length, 1);
    txml.txml_wasm_free(parserOut, 4, 4);
    txml.txml_wasm_free(config, configLayout.size, configLayout.align);
    txml.txml_wasm_free(allocator, allocatorLayout.size, allocatorLayout.align);
    return nodes;
  }

  const nodes = parse();
  assert.ok(nodes >= 8, `the document has its nodes (${nodes})`);
  for (let i = 0; i < 20; i++) assert.equal(parse(), nodes);
  const warm = txml.memory.buffer.byteLength;
  for (let i = 0; i < 500; i++) assert.equal(parse(), nodes);
  assert.equal(txml.memory.buffer.byteLength, warm, 'memory stops growing: freed blocks are reused');

  console.log(`wasm_test: txml ${version >> 16}.${(version >> 8) & 0xff}.${version & 0xff}: ` +
    `${nodes} nodes, 521 parses, memory steady at ${warm / 65536} pages`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
