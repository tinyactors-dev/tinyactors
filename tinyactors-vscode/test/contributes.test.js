'use strict';

// package.json's contributes hold together: the schemas are JSON Schema
// that lib/schema.js checks all of, every field a configuration or snippet
// writes is one its request declares, and the fields are the ones the two
// adapters read.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');
const schema = require('../lib/schema');
const { TYPE } = require('../lib/configuration');

const ROOT = path.resolve(__dirname, '..');
const REPOSITORY = path.resolve(ROOT, '..');
const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));
const [debuggerContribution] = manifest.contributes.debuggers;
const ATTRIBUTES = debuggerContribution.configurationAttributes;
// Fields VS Code reads itself, in every configuration.
const COMMON = ['type', 'request', 'name'];

// Every schema below `node`, with the path that reaches it.
function* schemas(node, where) {
  yield [node, where];
  for (const [name, entry] of Object.entries(node.properties ?? {})) yield* schemas(entry, `${where}.${name}`);
  if (node.items !== undefined) yield* schemas(node.items, `${where}[]`);
}

// Snippet bodies hold VS Code's snippet syntax (`${1:Workbench}`,
// `^"..."`) in place of values; only their field names are checked.
function fieldsOf(configuration) {
  return Object.keys(configuration).filter((name) => !COMMON.includes(name));
}

test('the manifest is an extension with no runtime dependencies', () => {
  assert.equal(manifest.name, 'tinyactors-vscode');
  assert.match(manifest.engines.vscode, /^\^1\.\d+\.\d+$/);
  assert.ok(fs.statSync(path.join(ROOT, manifest.main)).isFile(), 'main exists');
  assert.equal(manifest.dependencies, undefined, 'the extension has no runtime dependencies');
  assert.deepEqual(manifest.contributes.debuggers.length, 1);
  assert.equal(debuggerContribution.type, TYPE);
  assert.ok(manifest.activationEvents.includes(`onDebugResolve:${TYPE}`));
});

test('breakpoints are enabled in the languages charts are written in', () => {
  const languages = manifest.contributes.breakpoints.map((entry) => entry.language);
  for (const language of ['ruby', 'javascript', 'typescript', 'xml', 'scxml']) {
    assert.ok(languages.includes(language), language);
  }
});

test('every schema uses only JSON Schema keywords lib/schema.js checks, correctly formed', () => {
  const settings = manifest.contributes.configuration.properties;
  const roots = [
    ...Object.entries(ATTRIBUTES).map(([request, entry]) => [entry, request]),
    ...Object.entries(settings).map(([name, entry]) => [entry, name]),
  ];
  assert.deepEqual(Object.keys(ATTRIBUTES).sort(), ['attach', 'launch']);
  for (const [root, name] of roots) {
    for (const [node, where] of schemas(root, name)) {
      // A setting's `scope` is VS Code's, beside the schema.
      const unknown = Object.keys(node).filter((keyword) => !schema.KEYWORDS.includes(keyword)
        && !(node === root && settings[name] !== undefined && keyword === 'scope'));
      assert.deepEqual(unknown, [], `${where} uses only known keywords`);
      if (node.type !== undefined) {
        const types = [].concat(node.type);
        assert.ok(types.every((type) => schema.TYPES.includes(type)), `${where}.type`);
      }
      for (const name of node.required ?? []) {
        assert.ok(node.properties?.[name] !== undefined, `${where} requires ${name}, which it declares`);
      }
      if (node.enum !== undefined) {
        assert.ok(node.enum.length > 0 && new Set(node.enum).size === node.enum.length, `${where}.enum`);
      }
      if (node.enumDescriptions !== undefined) {
        assert.equal(node.enumDescriptions.length, node.enum.length, `${where}.enumDescriptions`);
      }
      if (node.pattern !== undefined) assert.doesNotThrow(() => new RegExp(node.pattern, 'u'), `${where}.pattern`);
      if (node.default !== undefined && !String(node.default).includes('${')) {
        assert.equal(schema.check(node.default, node, where), undefined, `${where}.default holds`);
      }
    }
  }
});

test('every field of a request and every setting is described', () => {
  const fields = [
    ...Object.entries(ATTRIBUTES).flatMap(([request, entry]) => Object.entries(entry.properties)
      .map(([name, field]) => [`${request}.${name}`, field])),
    ...Object.entries(manifest.contributes.configuration.properties),
  ];
  for (const [name, field] of fields) {
    assert.ok(field.description !== undefined || field.markdownDescription !== undefined, `${name} is described`);
  }
});

test('initial configurations hold and use only declared fields', () => {
  const requests = debuggerContribution.initialConfigurations.map((entry) => entry.request).sort();
  assert.deepEqual(requests, ['attach', 'launch'], 'one of each request');
  for (const configuration of debuggerContribution.initialConfigurations) {
    assert.equal(configuration.type, TYPE);
    const attributes = ATTRIBUTES[configuration.request];
    for (const field of fieldsOf(configuration)) {
      assert.ok(attributes.properties[field] !== undefined, `${configuration.request} declares ${field}`);
    }
    assert.equal(schema.check(configuration, attributes, ''), undefined, configuration.name);
  }
});

test('snippets use only declared fields and give every required one', () => {
  const requests = debuggerContribution.configurationSnippets.map((entry) => entry.body.request).sort();
  assert.deepEqual(requests, ['attach', 'launch'], 'one of each request');
  for (const { label, body } of debuggerContribution.configurationSnippets) {
    assert.equal(body.type, TYPE, label);
    const attributes = ATTRIBUTES[body.request];
    for (const field of fieldsOf(body)) {
      assert.ok(attributes.properties[field] !== undefined, `${label}: ${body.request} declares ${field}`);
    }
    for (const field of attributes.required ?? []) assert.ok(body[field] !== undefined, `${label} gives ${field}`);
  }
});

test('launch declares every argument the test host\'s adapter reads', () => {
  // DAP::Adapter#on_launch reads its arguments as args['...'].
  const source = fs.readFileSync(path.join(REPOSITORY, 'tinyactors-test/ruby/dap/adapter.rb'), 'utf8');
  const body = source.slice(source.indexOf('def on_launch'), source.indexOf('\n    end\n', source.indexOf('def on_launch')));
  const read = [...body.matchAll(/args\['(\w+)'\]/g)].map((match) => match[1]);
  assert.ok(read.length > 0, 'on_launch is found');
  // dapPath and runtimeExecutable are this extension's: they say how to
  // start the adapter, which ignores them.
  const own = ['dapPath', 'runtimeExecutable'];
  assert.deepEqual(Object.keys(ATTRIBUTES.launch.properties).filter((name) => !own.includes(name)).sort(),
    [...new Set(read)].sort());
});

test('attach offers the permissions napi\'s debug session knows', () => {
  const source = fs.readFileSync(path.join(REPOSITORY, 'tinyactors-napi/lib/debug.js'), 'utf8');
  const list = (name) => {
    const found = source.match(new RegExp(`^const ${name} = (\\[[^\\]]*\\]);`, 'm'));
    assert.ok(found, `${name} is found in lib/debug.js`);
    return JSON.parse(found[1].replaceAll("'", '"'));
  };
  const { properties } = ATTRIBUTES.attach;
  assert.equal(properties.mode, undefined, 'napi\'s session has no modes: it watches and pokes');
  assert.deepEqual(properties.allow.items.enum, list('PERMISSIONS'));
  for (const field of ['address', 'actor', 'sessionID']) {
    assert.match(source, new RegExp(`const \\{[^}]*\\b${field}\\b[^}]*\\} = args;`), `napi resolves ${field}`);
  }
});
