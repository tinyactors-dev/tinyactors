'use strict';

// The debugger demo: napi's example system (tools/dap-server.js), one live
// subscription actor watched and poked from VS Code. VS Code runs this as
// --extensionTestsPath; when run() returns, VS Code exits and the recording
// ends. Points of the display are for 1600x900 at window.zoomLevel 1.

const vscode = require('vscode');
const path = require('node:path');
const { sleep, fillScreen, click, caption, typeInConsole } = require('./stage');

// The top frame of the Call Stack, and the Debug Console's input line.
const TOP_FRAME = [175, 482];
const CONSOLE_INPUT = [900, 839];

// The live view refreshes the Call Stack without selecting its frame (it
// keeps the keyboard in the Debug Console), so the editor's current-line
// marker stays where it was. A viewer clicks the top frame to follow the
// actor, and clicks back into the console to type: so does the director.
async function followActor() {
  await click(...TOP_FRAME);
  await sleep(900);
  await click(...CONSOLE_INPUT);
}

async function run() {
  fillScreen();
  await sleep(800);
  const folder = vscode.workspace.workspaceFolders[0];
  const source = vscode.Uri.file(path.join(folder.uri.fsPath, 'tools/dap-server.js'));
  const document = await vscode.workspace.openTextDocument(source);
  const editor = await vscode.window.showTextDocument(document);
  editor.revealRange(new vscode.Range(36, 0, 36, 0), vscode.TextEditorRevealType.AtTop);
  await sleep(1500);
  await caption('The tinyactors debugger: watch and poke a live actor', 3500);

  const started = new Promise((resolve) => {
    const listener = vscode.debug.onDidStartDebugSession((session) => {
      listener.dispose();
      resolve(session);
    });
  });
  await vscode.debug.startDebugging(folder, {
    type: 'tinyactors', request: 'attach', name: 'acct_1 (production)',
    host: '127.0.0.1', port: 4711, address: 'subscription/acct_1',
  });
  await started;
  await sleep(1500);
  await vscode.commands.executeCommand('workbench.view.debug');
  await sleep(800);
  // Open the Data model scope, so its values are seen changing.
  await vscode.commands.executeCommand('workbench.debug.action.focusVariablesView');
  await sleep(300);
  await vscode.commands.executeCommand('list.focusFirst');
  await vscode.commands.executeCommand('list.expand');
  await sleep(800);
  await click(...CONSOLE_INPUT);
  await caption('Attached to subscription/acct_1: it keeps running', 4500);

  await caption('Live traffic: every macrostep shows up, and the view refreshes', 3500);
  await sleep(6000);
  await followActor();
  await sleep(2500);

  await caption('Poke it: send an event from the console', 3000);
  await typeInConsole('send payment.failed');
  await sleep(3000);
  await followActor();
  await sleep(1500);
  await typeInConsole('send payment.succeeded');
  await sleep(3000);
  await followActor();
  await sleep(1500);

  await caption('The chart replies to whoever sent the event: the console', 3500);
  await typeInConsole('send ping');
  await sleep(4000);

  await caption('Breakpoints never stop a live actor: they say where they would have', 3500);
  vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(source, new vscode.Position(44, 0)))]);
  editor.revealRange(new vscode.Range(40, 0, 40, 0), vscode.TextEditorRevealType.AtTop);
  await sleep(1500);
  await typeInConsole('send payment.failed');
  await sleep(3500);
  await followActor();
  await sleep(1500);

  await caption('Unwedge it: jump to any state, through the normal engine', 3500);
  // Random traffic may have moved it on: put it back in past_due first, so
  // the goto is seen to move it.
  await typeInConsole('send payment.failed');
  await sleep(2000);
  await followActor();
  await sleep(1000);
  await typeInConsole('goto active');
  await sleep(3000);
  await followActor();
  await sleep(1500);
  await typeInConsole('failures');
  await sleep(3000);

  await caption('Withdraw a pending timer: the renewal it scheduled', 3500);
  await typeInConsole('cancel renewal');
  await sleep(4000);

  await caption('No production actor is ever paused', 3500);
  await vscode.debug.stopDebugging();
  await sleep(1500);
}

module.exports = { run };
