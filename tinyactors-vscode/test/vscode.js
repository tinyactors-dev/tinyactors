'use strict';

// A stub of the `vscode` module, as much of it as lib/extension.js uses,
// so its tests run under Node without VS Code. Requiring this file makes
// `require('vscode')` answer the stub; `reset()` sets the workspace a test
// sees and clears what the extension registered and showed.

const Module = require('node:module');

class DebugAdapterExecutable {
  constructor(command, args, options) {
    this.command = command;
    this.args = args;
    this.options = options;
  }
}

class DebugAdapterServer {
  constructor(port, host) {
    this.port = port;
    this.host = host;
  }
}

const state = {
  providers: new Map(),
  factories: new Map(),
  errors: [],
  settings: {},
};

function folder(fsPath) {
  return { uri: { fsPath }, name: fsPath };
}

const vscode = {
  DebugAdapterExecutable,
  DebugAdapterServer,
  debug: {
    registerDebugConfigurationProvider(type, provider) {
      state.providers.set(type, provider);
      return { dispose: () => state.providers.delete(type) };
    },
    registerDebugAdapterDescriptorFactory(type, factory) {
      state.factories.set(type, factory);
      return { dispose: () => state.factories.delete(type) };
    },
  },
  window: {
    activeTextEditor: undefined,
    showErrorMessage(message) {
      state.errors.push(message);
      return Promise.resolve(undefined);
    },
  },
  workspace: {
    workspaceFolders: undefined,
    getConfiguration(section) {
      return { get: (name) => state.settings[`${section}.${name}`] };
    },
  },
};

// `folders`: workspace folder paths; `activeFile`: the path of the file in
// the active editor; `settings`: { 'tinyactors.dapPath': ... }.
function reset({ folders = [], activeFile, settings = {} } = {}) {
  vscode.workspace.workspaceFolders = folders.length === 0 ? undefined : folders.map(folder);
  vscode.window.activeTextEditor = activeFile === undefined ? undefined
    : { document: { fileName: activeFile, isUntitled: false } };
  state.settings = settings;
  state.errors = [];
  state.providers.clear();
  state.factories.clear();
}

const resolve = Module._resolveFilename;
Module._resolveFilename = function resolveVSCode(request, ...rest) {
  return request === 'vscode' ? request : resolve.call(this, request, ...rest);
};
require.cache.vscode = { id: 'vscode', filename: 'vscode', loaded: true, exports: vscode };

module.exports = { vscode, state, reset, folder };
