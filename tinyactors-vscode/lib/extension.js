'use strict';

// Registers the `tinyactors` debug type. VS Code starts a debug adapter
// only for a type an extension registers; this one has two:
//
// - `launch` runs the test host's adapter (tinyactors-test/bin/dap) over
//   stdio, which runs a workbench directory;
// - `attach` connects to an application that serves debug sessions over
//   TCP (tinyactors-napi's createDebugSession behind a transport, such as
//   tools/dap-server.js --port).
//
// Every field of the configuration goes to the adapter as the `launch` or
// `attach` arguments; lib/configuration.js fills defaults and refuses a
// configuration the adapter could not use.

const fs = require('node:fs');
const vscode = require('vscode');
const configuration = require('./configuration');

function isKind(file, kind) {
  try {
    return fs.statSync(file)[kind]();
  } catch {
    return false;
  }
}

function place(folder) {
  const editor = vscode.window.activeTextEditor;
  const document = editor === undefined ? undefined : editor.document;
  const settings = vscode.workspace.getConfiguration('tinyactors', folder === undefined ? undefined : folder.uri);
  // The folder the configuration belongs to comes first: relative paths
  // are relative to it.
  const own = folder === undefined ? [] : [folder.uri.fsPath];
  const others = (vscode.workspace.workspaceFolders ?? []).map((entry) => entry.uri.fsPath)
    .filter((entry) => !own.includes(entry));
  return {
    folders: [...own, ...others],
    activeFile: document === undefined || document.isUntitled ? undefined : document.fileName,
    dapPath: settings.get('dapPath'),
    isFile: (file) => isKind(file, 'isFile'),
    isDirectory: (file) => isKind(file, 'isDirectory'),
  };
}

// Shows a refusal and answers undefined, which ends the start of the
// session without opening launch.json.
function answer({ configuration: resolved, problem }) {
  if (problem === undefined) return resolved;
  vscode.window.showErrorMessage(`Tinyactors: ${problem}`);
  return undefined;
}

const provider = {
  resolveDebugConfiguration(folder, debugConfiguration) {
    return answer(configuration.complete(debugConfiguration, place(folder)));
  },
  resolveDebugConfigurationWithSubstitutedVariables(folder, debugConfiguration) {
    return answer(configuration.settle(debugConfiguration, place(folder)));
  },
};

const factory = {
  createDebugAdapterDescriptor(session) {
    const described = configuration.describe(session.configuration);
    if (described.kind === 'server') return new vscode.DebugAdapterServer(described.port, described.host);
    return new vscode.DebugAdapterExecutable(described.command, described.args);
  },
};

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider(configuration.TYPE, provider),
    vscode.debug.registerDebugAdapterDescriptorFactory(configuration.TYPE, factory),
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
