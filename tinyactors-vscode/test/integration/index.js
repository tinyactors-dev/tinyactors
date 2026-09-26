'use strict';

// Runs inside VS Code's extension host (`mise run integration`), with this
// extension loaded from source: a launch runs the test host's adapter on a
// workbench, an attach reaches napi's development server, and a broken
// configuration starts nothing. VS Code runs `run()` and exits with its
// outcome.

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vscode = require('vscode');

const REPOSITORY = path.resolve(__dirname, '../../..');
const TIMEOUT = 60_000;

const COUNTER = `TinyactorsTest.chart :counter do
  data count: 0
  initial :idle
  state :idle do
    transition on: 'bump', type: :internal do
      assign '.count', '.count + 1'
    end
  end
end
`;

// What each session's adapter sent, by session ID.
const sent = new Map();

function received(session, test) {
  return (sent.get(session.id) ?? []).find(test);
}

async function until(what, test) {
  const deadline = Date.now() + TIMEOUT;
  for (;;) {
    const found = test();
    if (found) return found;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((resolve) => { setTimeout(resolve, 50); });
  }
}

// Starts a session and answers it once VS Code has it.
async function start(configuration) {
  const started = new Promise((resolve) => {
    const listener = vscode.debug.onDidStartDebugSession((session) => {
      listener.dispose();
      resolve(session);
    });
  });
  assert.equal(await vscode.debug.startDebugging(undefined, configuration), true, `${configuration.name} starts`);
  return started;
}

async function stop(session) {
  const ended = new Promise((resolve) => {
    const listener = vscode.debug.onDidTerminateDebugSession((terminated) => {
      if (terminated.id !== session.id) return;
      listener.dispose();
      resolve();
    });
  });
  await vscode.debug.stopDebugging(session);
  await ended;
}

async function launchesAWorkbench() {
  const workbench = fs.mkdtempSync(path.join(os.tmpdir(), 'tinyactors-vscode-workbench-'));
  fs.writeFileSync(path.join(workbench, 'counter.rb'), COUNTER);
  fs.writeFileSync(path.join(workbench, 'actors.rb'), 'Workbench.actor :acct1, chart: :counter\n');
  const session = await start({
    type: 'tinyactors',
    request: 'launch',
    name: 'Workbench',
    workbench,
    stopOnEntry: true,
    dapPath: path.join(REPOSITORY, 'tinyactors-test', 'bin', 'dap'),
  });
  await until('the stop on entry', () => received(session, (message) => message.event === 'stopped'));
  const { threads } = await session.customRequest('threads');
  assert.ok(threads.some((thread) => thread.name.includes('acct1')), JSON.stringify(threads));
  await stop(session);
}

async function attachesToAnApplication() {
  // The Node.js on PATH (mise's): the addon needs Node.js 22, not VS Code's.
  const server = spawn('node', ['tools/dap-server.js', '--port', '0', '--mode', 'fork'], {
    cwd: path.join(REPOSITORY, 'tinyactors-napi'),
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  let stderr = '';
  server.stderr.on('data', (bytes) => { stderr += bytes; });
  try {
    const port = await until('the server to listen', () => stderr.match(/sessions on 127\.0\.0\.1:(\d+)/)?.[1]);
    const session = await start({
      type: 'tinyactors',
      request: 'attach',
      name: 'Attach',
      port: Number(port),
      address: 'subscription/acct_1',
      mode: 'fork',
    });
    await until('the process event', () => received(session, (message) => message.event === 'process'));
    const { threads } = await session.customRequest('threads');
    assert.ok(threads.some((thread) => thread.name.includes('acct_1')), JSON.stringify(threads));
    await stop(session);
  } finally {
    server.kill();
  }
}

async function refusesABrokenConfiguration() {
  const started = await vscode.debug.startDebugging(undefined, {
    type: 'tinyactors', request: 'attach', name: 'Broken', port: 0, address: 'subscription/acct_1',
  });
  assert.equal(started, false);
}

async function run() {
  vscode.debug.registerDebugAdapterTrackerFactory('tinyactors', {
    createDebugAdapterTracker(session) {
      sent.set(session.id, []);
      return { onDidSendMessage: (message) => sent.get(session.id).push(message) };
    },
  });
  for (const check of [launchesAWorkbench, attachesToAnApplication, refusesABrokenConfiguration]) {
    console.log(`integration: ${check.name}`);
    await check();
  }
  console.log('integration: passed');
}

module.exports = { run };
