# Tinyactors for VS Code

Registers the `tinyactors` debug type, so VS Code starts the Tinyactors
debug adapters itself: no `debugServer` in your configurations. It has
two requests:

- **`launch`** runs a directory of charts as a workbench with the test
  host's adapter, `tinyactors-test/bin/dap`, over stdin and stdout
  ([its README](../tinyactors-test/ruby/dap/README.md)).
- **`attach`** connects to an application that serves debug sessions over
  TCP, and attaches to one of its actors: tinyactors-napi's
  `createDebugSession` behind a socket
  ([its README](../tinyactors-napi/README.md#debugging-an-actor)).

The extension is only this registration and the checks of a
configuration; what each Debug Adapter Protocol message does is in [the
status map](../docs/debug-adapter.md). Other clients (nvim-dap, Emacs
dape) need no extension: the adapters' READMEs show their configuration.

## Install

```text
cd tinyactors-vscode
mise run build        # writes build/tinyactors-vscode.vsix
code --install-extension build/tinyactors-vscode.vsix
```

`mise run package` is the same task. The package is built without `vsce`
or any download, and the extension has no dependencies. It needs VS Code
1.80 or later.

To work on the extension, open this directory in VS Code and run it with
**Run Extension** (`--extensionDevelopmentPath`), or link it into
`~/.vscode/extensions/tinyactors.tinyactors-vscode-0.0.0` and reload.

## Launching a workbench

```json
{
  "type": "tinyactors",
  "request": "launch",
  "name": "Workbench",
  "workbench": "${workspaceFolder}/workbench",
  "stopOnEntry": true
}
```

| Field | |
|---|---|
| `workbench` (or `program`) | the directory of chart files; required |
| `stopOnEntry` | stop once every actor has started, before any event reaches one |
| `host` | the `tinyactors-test` executable, when not the one beside `bin/dap` |
| `dapPath` | the adapter; default: the `tinyactors.dapPath` setting, else `tinyactors-test/bin/dap` (or `bin/dap`) in a workspace folder or a directory above the workbench |
| `runtimeExecutable` | the Ruby 4 that runs `bin/dap`, when the `ruby` on VS Code's `PATH` is not it |

`bin/dap` needs the host built (`mise run build` in tinyactors-test) and
Ruby 4 on the `PATH`. VS Code started from a desktop launcher may not see
mise's Ruby: start it from a shell where mise is active, or set
`runtimeExecutable` to the Ruby that `mise which ruby` prints in
tinyactors-test.

With no `launch.json`, F5 in a chart file launches that file's directory
as a workbench, stopped on entry.

## Attaching to an actor

```json
{
  "type": "tinyactors",
  "request": "attach",
  "name": "Attach to acct_1",
  "host": "127.0.0.1",
  "port": 4711,
  "address": "subscription/acct_1",
  "allow": ["send", "goto"]
}
```

| Field | |
|---|---|
| `port` | the port the application serves debug sessions on; required |
| `host` | its host; default `127.0.0.1` |
| `address` | the actor, `"family/name"` or `{ "family", "name" }` |
| `sessionID` | or the actor by its session ID, in decimal |
| `actor` | or by its actor ID, `{ "slot", "generation" }` |
| `allow` | the permissions to ask for, of `send`, `set`, `goto`, `evaluate`; none watches only; default and most: what the session grants |

The actor runs in production and is never paused. The session reports it
stopped once, on entry, so VS Code shows its stack and variables, and
refreshes them after each of its macrosteps. A poke from the console
(`send`, `goto`, `abandon`, `cancel`) causes a macrostep, which shows the
same way; setting a variable writes the data model at once. Breakpoints are tracepoints: the Debug
Console says where one would have stopped. Stepping and pausing are
refused (napi ADR 0027).

To try it, `mise run dap-server -- --port 4711` in tinyactors-napi serves
sessions over a small example system with every permission granted, and
the configuration above attaches to one of its actors.

Everything in the configuration goes to the adapter as the `launch` or
`attach` arguments. The extension checks it first against the schema in
`package.json`, and refuses to start a session whose fields are missing
or of the wrong kind, with a message that names the field.

## Breakpoints

Breakpoints can be set in Ruby, JavaScript, TypeScript and XML files,
the languages charts are written in; the adapter verifies each against the
charts it runs. An `.scxml` file takes breakpoints when VS Code opens it
as XML: add `"files.associations": { "*.scxml": "xml" }`.

## Settings

| Setting | |
|---|---|
| `tinyactors.dapPath` | the test host's adapter for a `launch`; relative to the workspace folder |

## Tests

`mise run test` builds the package and runs the extension under a stub of
the `vscode` module (`test/vscode.js`): what activation registers, the
adapter each configuration reaches, how one is completed or refused,
that `package.json`'s contributes hold together and name the fields the
two adapters read, and the contents of the `.vsix`.

`mise run integration` runs the extension in a real VS Code, downloaded
once into `build/` (`-- --version latest` for the newest; the default is
1.80.0, the oldest it supports): it installs the `.vsix`, launches a
workbench, attaches to napi's development server, and checks that a
broken configuration starts nothing. It needs `xvfb-run`, and the
repository built. It is not part of `mise run test`.

## How it is put together

| File | |
|---|---|
| `package.json` | the debug type, its configuration schemas, snippets and setting |
| `lib/extension.js` | activation: the configuration provider and the adapter descriptor factory |
| `lib/configuration.js` | defaults, checks, finding `bin/dap`, and which adapter a configuration reaches |
| `lib/schema.js` | the part of JSON Schema the configuration schemas use |
| `mise/tasks/build` | the `.vsix` |
