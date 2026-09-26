'use strict';

// Debug configurations of type `tinyactors`, without VS Code: defaults for
// a missing configuration, the checks that refuse a broken one, where the
// test host's adapter is, and how to reach the adapter. lib/extension.js
// gives these functions what they need to know about the workspace (a
// `place`) and turns their answers into VS Code objects.
//
// A place is
//   { folders: [path], activeFile: path | undefined, dapPath: string | undefined,
//     isFile(path), isDirectory(path) }
// where `dapPath` is the `tinyactors.dapPath` setting as written.

const path = require('node:path');
const manifest = require('../package.json');
const schema = require('./schema');

const TYPE = 'tinyactors';
const [debuggerContribution] = manifest.contributes.debuggers;
const ATTRIBUTES = debuggerContribution.configurationAttributes;

// Where tinyactors-test is, relative to a directory that may hold it: the
// repository's root, or tinyactors-test itself. It is known by bin/dap
// beside the Ruby adapter it loads, so another project's bin/dap is not
// taken for it.
const TEST_HOST_CANDIDATES = ['tinyactors-test', '.'];
const DEFAULT_HOST = '127.0.0.1';

// `resolveDebugConfiguration`: runs before VS Code substitutes variables.
// An empty configuration (F5 with no launch.json) becomes a launch of the
// active file's directory as a workbench. Answers { configuration } or
// { problem }.
function complete(configuration, place) {
  if (configuration.type === undefined && configuration.request === undefined && configuration.name === undefined) {
    if (place.activeFile === undefined && place.folders.length === 0) {
      return { problem: 'Open a workbench folder or a chart file, or add a "tinyactors" configuration to launch.json.' };
    }
    return {
      configuration: {
        type: TYPE,
        request: 'launch',
        name: 'Workbench',
        workbench: place.activeFile !== undefined ? '${fileDirname}' : '${workspaceFolder}',
        stopOnEntry: true,
      },
    };
  }
  if (configuration.request === 'attach' && configuration.host === undefined) {
    return { configuration: { ...configuration, host: DEFAULT_HOST } };
  }
  return { configuration };
}

// `resolveDebugConfigurationWithSubstitutedVariables`: checks the
// configuration against its request's schema and what the request needs,
// and for a launch finds the adapter. Answers { configuration } or
// { problem }.
function settle(configuration, place) {
  const attributes = ATTRIBUTES[configuration.request];
  if (attributes === undefined) {
    return { problem: `"request" is "launch" or "attach", not ${JSON.stringify(configuration.request)}.` };
  }
  const broken = schema.check(configuration, attributes, '');
  if (broken !== undefined) return { problem: `${broken}.` };
  return configuration.request === 'launch' ? settleLaunch(configuration, place) : settleAttach(configuration);
}

function settleLaunch(configuration, place) {
  const base = place.folders[0] ?? process.cwd();
  const key = configuration.workbench !== undefined ? 'workbench' : 'program';
  if (configuration[key] === undefined) return { problem: 'A launch needs "workbench": the directory of chart files.' };
  // The adapter's working directory is not the workspace's: it gets
  // absolute paths.
  const workbench = path.resolve(base, configuration[key]);
  if (!place.isDirectory(workbench)) return { problem: `The workbench ${workbench} is not a directory.` };
  const settled = { ...configuration, [key]: workbench };
  if (configuration.host !== undefined) {
    settled.host = path.resolve(base, configuration.host);
    if (!place.isFile(settled.host)) return { problem: `The host ${settled.host} is not a file.` };
  }
  const written = [configuration.dapPath, place.dapPath].find((entry) => entry !== undefined && entry !== '');
  if (written !== undefined) {
    settled.dapPath = path.resolve(base, written);
    if (!place.isFile(settled.dapPath)) return { problem: `The adapter ${settled.dapPath} is not a file.` };
    return { configuration: settled };
  }
  settled.dapPath = findDAP([...place.folders, ...ancestors(workbench)], place);
  if (settled.dapPath === undefined) {
    return {
      problem: 'Cannot find tinyactors-test/bin/dap in the workspace or above the workbench: set "dapPath" '
        + 'in the configuration, or the "tinyactors.dapPath" setting.',
    };
  }
  return { configuration: settled };
}

function settleAttach(configuration) {
  if (configuration.address === undefined && configuration.actor === undefined && configuration.sessionID === undefined) {
    return { problem: 'An attach needs the actor: "address", "sessionID" or "actor".' };
  }
  return { configuration };
}

// When neither the configuration nor the setting says where the adapter
// is: bin/dap in the first of `directories` that holds it, as the
// repository's root or as tinyactors-test itself.
function findDAP(directories, place) {
  for (const directory of directories) {
    for (const candidate of TEST_HOST_CANDIDATES) {
      const testHost = path.join(directory, candidate);
      const found = path.join(testHost, 'bin', 'dap');
      if (place.isFile(found) && place.isFile(path.join(testHost, 'ruby', 'dap.rb'))) return found;
    }
  }
  return undefined;
}

function ancestors(directory) {
  const found = [];
  for (let at = directory; ; at = path.dirname(at)) {
    found.push(at);
    if (path.dirname(at) === at) return found;
  }
}

// `createDebugAdapterDescriptor`: a launch runs the test host's adapter
// over stdio (through "runtimeExecutable" when one is given, since
// bin/dap needs Ruby 4); an attach connects to the port the application
// serves sessions on.
function describe(configuration) {
  if (configuration.request === 'attach') {
    return { kind: 'server', port: configuration.port, host: configuration.host ?? DEFAULT_HOST };
  }
  if (configuration.dapPath === undefined) {
    throw new Error('The configuration was not resolved: it has no "dapPath".');
  }
  if (configuration.runtimeExecutable !== undefined) {
    return { kind: 'executable', command: configuration.runtimeExecutable, args: [configuration.dapPath] };
  }
  return { kind: 'executable', command: configuration.dapPath, args: [] };
}

module.exports = { TYPE, complete, settle, describe };
