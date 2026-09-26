'use strict';

// The activation code under a stub of `vscode` (test/vscode.js): what it
// registers, the adapter each configuration reaches, and how a
// configuration is completed, settled or refused.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test, beforeEach } = require('node:test');
const { vscode, state, reset, folder } = require('./vscode');
const extension = require('../lib/extension');

const REPOSITORY = path.resolve(__dirname, '../..');

// A directory laid out like the repository: a workbench, and
// tinyactors-test's bin/dap beside the adapter it loads.
function repository() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tinyactors-vscode-'));
  for (const directory of ['bin', 'ruby']) fs.mkdirSync(path.join(root, 'tinyactors-test', directory), { recursive: true });
  fs.writeFileSync(path.join(root, 'tinyactors-test', 'bin', 'dap'), '#!/usr/bin/env ruby\n');
  fs.writeFileSync(path.join(root, 'tinyactors-test', 'ruby', 'dap.rb'), '');
  fs.mkdirSync(path.join(root, 'charts', 'billing'), { recursive: true });
  return root;
}

function activate(workspace) {
  reset(workspace);
  const context = { subscriptions: [] };
  extension.activate(context);
  return context;
}

function provider() {
  return state.providers.get('tinyactors');
}

function descriptor(configuration) {
  return state.factories.get('tinyactors').createDebugAdapterDescriptor({ configuration });
}

// Both steps VS Code takes, with no variables to substitute between them.
function resolve(configuration, owner) {
  const completed = provider().resolveDebugConfiguration(owner, configuration);
  return completed === undefined ? undefined
    : provider().resolveDebugConfigurationWithSubstitutedVariables(owner, completed);
}

function refused(configuration, owner, message) {
  assert.equal(resolve(configuration, owner), undefined);
  assert.equal(state.errors.length, 1, 'one error is shown');
  assert.match(state.errors[0], message);
}

beforeEach(() => reset());

test('activation registers a configuration provider and a descriptor factory for tinyactors', () => {
  const context = activate();
  assert.deepEqual([...state.providers.keys()], ['tinyactors']);
  assert.deepEqual([...state.factories.keys()], ['tinyactors']);
  assert.equal(context.subscriptions.length, 2);
  for (const subscription of context.subscriptions) subscription.dispose();
  assert.equal(state.providers.size + state.factories.size, 0);
});

test('a launch runs the adapter over stdio, through the runtime when one is given', () => {
  activate();
  const direct = descriptor({ type: 'tinyactors', request: 'launch', workbench: '/w', dapPath: '/r/bin/dap' });
  assert.ok(direct instanceof vscode.DebugAdapterExecutable);
  assert.equal(direct.command, '/r/bin/dap');
  assert.deepEqual(direct.args, []);

  const ruby = descriptor({ request: 'launch', workbench: '/w', dapPath: '/r/bin/dap', runtimeExecutable: '/ruby' });
  assert.equal(ruby.command, '/ruby');
  assert.deepEqual(ruby.args, ['/r/bin/dap']);

  assert.throws(() => descriptor({ request: 'launch', workbench: '/w' }), /no "dapPath"/);
});

test('an attach connects to the application at host and port', () => {
  activate();
  const server = descriptor({ type: 'tinyactors', request: 'attach', host: '10.0.0.2', port: 9229, address: 'a/b' });
  assert.ok(server instanceof vscode.DebugAdapterServer);
  assert.equal(server.port, 9229);
  assert.equal(server.host, '10.0.0.2');
  assert.equal(descriptor({ request: 'attach', port: 4711, address: 'a/b' }).host, '127.0.0.1');
});

test('with no launch.json, F5 launches the active file\'s directory, else the workspace folder', () => {
  const root = repository();
  activate({ folders: [root], activeFile: path.join(root, 'charts', 'billing', 'invoice.rb') });
  assert.deepEqual(provider().resolveDebugConfiguration(folder(root), {}), {
    type: 'tinyactors', request: 'launch', name: 'Workbench', workbench: '${fileDirname}', stopOnEntry: true,
  });

  activate({ folders: [root] });
  assert.equal(provider().resolveDebugConfiguration(folder(root), {}).workbench, '${workspaceFolder}');

  activate();
  assert.equal(provider().resolveDebugConfiguration(undefined, {}), undefined);
  assert.match(state.errors[0], /Open a workbench folder/);
});

test('a launch finds bin/dap in a workspace folder and makes paths absolute', () => {
  const root = repository();
  activate({ folders: [root] });
  const resolved = resolve({ type: 'tinyactors', request: 'launch', name: 'W', workbench: 'charts', stopOnEntry: true },
    folder(root));
  assert.deepEqual(resolved, {
    type: 'tinyactors',
    request: 'launch',
    name: 'W',
    workbench: path.join(root, 'charts'),
    stopOnEntry: true,
    dapPath: path.join(root, 'tinyactors-test', 'bin', 'dap'),
  });
  assert.deepEqual(state.errors, []);

  const program = resolve({ type: 'tinyactors', request: 'launch', name: 'W', program: 'charts' }, folder(root));
  assert.equal(program.program, path.join(root, 'charts'));
  assert.equal(program.workbench, undefined);
});

test('a launch finds bin/dap above the workbench when no workspace folder has it', () => {
  const root = repository();
  const elsewhere = fs.mkdtempSync(path.join(os.tmpdir(), 'tinyactors-vscode-'));
  activate({ folders: [elsewhere] });
  const workbench = path.join(root, 'charts', 'billing');
  const resolved = resolve({ type: 'tinyactors', request: 'launch', name: 'W', workbench }, folder(elsewhere));
  assert.equal(resolved.dapPath, path.join(root, 'tinyactors-test', 'bin', 'dap'));

  // Inside tinyactors-test itself, bin/dap is the folder's own.
  activate({ folders: [path.join(REPOSITORY, 'tinyactors-test')] });
  const own = resolve({ type: 'tinyactors', request: 'launch', name: 'W', workbench: 'ruby' },
    folder(path.join(REPOSITORY, 'tinyactors-test')));
  assert.equal(own.dapPath, path.join(REPOSITORY, 'tinyactors-test', 'bin', 'dap'));
});

test('a launch takes the adapter from its configuration, else from the setting', () => {
  const root = repository();
  const other = repository();
  const dap = path.join(other, 'tinyactors-test', 'bin', 'dap');
  activate({ folders: [root], settings: { 'tinyactors.dapPath': dap } });
  const configured = { type: 'tinyactors', request: 'launch', name: 'W', workbench: root };
  assert.equal(resolve(configured, folder(root)).dapPath, dap);

  activate({ folders: [other], settings: { 'tinyactors.dapPath': 'tinyactors-test/bin/dap' } });
  assert.equal(resolve(configured, folder(other)).dapPath, dap, 'relative to the folder');

  activate({ folders: [root], settings: { 'tinyactors.dapPath': '/nowhere/dap' } });
  const own = resolve({ ...configured, dapPath: dap }, folder(root));
  assert.equal(own.dapPath, dap, 'the configuration wins over the setting');
  refused(configured, folder(root), /The adapter \/nowhere\/dap is not a file/);
});

test('a launch is refused when its workbench, host or adapter is missing', () => {
  const root = repository();
  activate({ folders: [root] });
  const owner = folder(root);
  const launch = { type: 'tinyactors', request: 'launch', name: 'W' };
  refused(launch, owner, /A launch needs "workbench"/);
  activate({ folders: [root] });
  refused({ ...launch, workbench: 'nowhere' }, owner, /nowhere is not a directory/);
  activate({ folders: [root] });
  refused({ ...launch, workbench: root, host: 'build/tinyactors-test' }, owner, /The host .* is not a file/);
  activate({ folders: [root] });
  refused({ ...launch, workbench: root, stopOnEntry: 'yes' }, owner, /"stopOnEntry" is boolean, not "yes"/);
  activate({ folders: [root] });
  refused({ ...launch, workbench: '' }, owner, /"workbench" is empty/);

  // Another project's bin/dap is not the test host's.
  const other = fs.mkdtempSync(path.join(os.tmpdir(), 'tinyactors-vscode-'));
  fs.mkdirSync(path.join(other, 'bin'));
  fs.writeFileSync(path.join(other, 'bin', 'dap'), '');
  activate({ folders: [other] });
  refused({ ...launch, workbench: other }, folder(other), /Cannot find tinyactors-test\/bin\/dap/);
});

test('an attach passes its fields through and fills the host', () => {
  activate();
  const attach = {
    type: 'tinyactors',
    request: 'attach',
    name: 'A',
    port: 4711,
    address: 'subscription/acct_1',
    allow: ['send', 'goto'],
  };
  assert.deepEqual(resolve(attach), { ...attach, host: '127.0.0.1' });
  assert.deepEqual(resolve({ ...attach, host: 'example.test', address: { family: 'f', name: 'n' } }),
    { ...attach, host: 'example.test', address: { family: 'f', name: 'n' } });
  assert.equal(resolve({ ...attach, address: undefined, sessionID: '42' }).sessionID, '42');
  assert.deepEqual(resolve({ ...attach, address: undefined, actor: { slot: 3, generation: 1 } }).actor,
    { slot: 3, generation: 1 });
  assert.deepEqual(state.errors, []);
});

test('an attach is refused when its fields would not reach an actor', () => {
  const attach = { type: 'tinyactors', request: 'attach', name: 'A', port: 4711, address: 'a/b' };
  const cases = [
    [{ ...attach, port: undefined }, /"port" is required/],
    [{ ...attach, port: 0 }, /"port" is at least 1/],
    [{ ...attach, port: '4711' }, /"port" is integer, not "4711"/],
    [{ ...attach, address: undefined }, /needs the actor: "address", "sessionID" or "actor"/],
    [{ ...attach, address: 'nobody' }, /"address" does not match/],
    [{ ...attach, address: { family: 'f' } }, /"address.name" is required/],
    [{ ...attach, address: { family: 'f', name: 'n', revision: 2 } }, /"address.revision" is not a field/],
    [{ ...attach, address: undefined, sessionID: 'x1' }, /"sessionID" does not match/],
    [{ ...attach, address: undefined, actor: { slot: -1, generation: 0 } }, /"actor.slot" is at least 0/],
    [{ ...attach, allow: ['send', 'send'] }, /"allow" lists an entry twice/],
    [{ ...attach, allow: ['send', 'kill'] }, /"allow\[1\]" is one of/],
    [{ ...attach, request: 'connect' }, /"request" is "launch" or "attach", not "connect"/],
  ];
  for (const [configuration, message] of cases) {
    activate();
    refused(configuration, undefined, message);
  }
});
