---
name: record-demo-video
description: Record a demo video of the tinyactors VS Code extension driving a debug session, on a headless Linux machine (Xvfb, ffmpeg, xdotool). Use when asked for a video, screencast or recording of the debugger or the extension in action.
---

# Recording a demo video

A demo is VS Code on a virtual X display, recorded with ffmpeg, while a
*director* script plays the scenario inside VS Code's own extension host.
Nothing is clicked by hand, so a take is repeatable: change the director,
record again.

```text
scripts/record.sh <output.mp4>
```

records the debugger demo: napi's example system (`tinyactors-napi/tools/dap-server.js`,
three live subscription actors with random traffic), VS Code attached to
`subscription/acct_1`, and the console poking it. A take lasts about 100
seconds and runs for about two minutes.

## Prerequisites

- `apt-get install -y xvfb ffmpeg xdotool curl`
- the repository's tools (`mise install`) and napi built
  (`mise run //tinyactors-napi:build`): the example server is napi.
- network access to `update.code.visualstudio.com` the first time: VS Code
  (`VSCODE_VERSION`, 1.139.1 by default) is downloaded into
  `tinyactors-vscode/build/`, which git ignores.

## How it works

`scripts/record.sh` starts, in order:

1. `Xvfb :77` at `DEMO_SIZE` (1600x900);
2. the program to debug (`DEMO_SERVER`, run in `DEMO_WORKSPACE`);
3. `ffmpeg -f x11grab` at 15 frames per second;
4. VS Code, with `--extensionDevelopmentPath=tinyactors-vscode` (the
   extension under test) and `--extensionTestsPath=<director>`: VS Code
   loads the director as if it were a test suite, calls its `run()`, and
   exits when it returns. That ends the take.

Then it stops the rest, cuts `DEMO_TRIM` seconds of VS Code starting off
the front, and writes the video. The raw recording and the logs
(`server.log`, `code.log`) stay in the temporary directory it names.

VS Code starts from an empty user data directory with `scripts/settings.json`
(dark theme, zoom 1, no git, no telemetry, no minimap, no AI features), so
every take looks the same.

## Writing a director

A director is a directory with an `index.js` that exports `async run()`.
It has the whole `vscode` API. `scripts/director/stage.js` has what every
scene needs:

| Helper | |
|---|---|
| `fillScreen()` | Xvfb has no window manager: moves the window to the corner and sizes it to the display |
| `caption(text, ms)` | a notification as a caption, cleared after `ms` |
| `typeInConsole(text)` | types a Debug Console line one character at a time, then runs it |
| `click(x, y)` | a real pointer click, through xdotool |
| `sleep(ms)` | |

Use a command (`vscode.commands.executeCommand`) wherever one exists:
`workbench.view.debug`, `workbench.debug.action.focusRepl`,
`workbench.debug.action.focusVariablesView`, `list.focusFirst`,
`list.expand`, `editor.revealRange`. Use `click` for what no command does,
such as selecting a Call Stack frame.

To use another director, put it anywhere and pass `DEMO_DIRECTOR=<dir>`; it
can `require` the stage from this skill.

## Finding where to click

Points are pixels of the display, and depend on its size, the zoom and
which views are open. Find them from a frame of a take:

```text
ffmpeg -ss 30 -i demo.mp4 -frames:v 1 frame.png     # then look at frame.png
```

In the debugger demo (1600x900, zoom 1, Run and Debug view open), the top
Call Stack frame is at (175, 482) and the Debug Console input at (900, 839).

## Checking a take

Watch it as a contact sheet before handing it over:

```text
ffmpeg -i demo.mp4 -vf "fps=1/4,scale=800:-1,tile=2x3" sheet%d.png
```

and look for: text typed into the editor instead of the console (the focus
moved), a suggest widget left open, captions piling up, an empty Variables
view, and scenes where the program's own activity hides what the scene is
showing. Then check that `git status` is clean: nothing the take ran may
change tracked files.

## Pitfalls

These went wrong once:

- **The IPC socket path is too long.** VS Code puts a Unix socket in its
  user data directory, and a socket path may be at most 107 bytes. Keep
  that directory short: `record.sh` uses `/tmp/vsd.XXXX`.
- **Typing goes to the editor.** Anything that takes the focus while the
  director types sends the rest of the line to the editor:
  `showTextDocument`, or a debug stop that asks for focus. Scroll with
  `editor.revealRange` instead, and focus the console again before
  running the line (`typeInConsole` does).
- **A suggest widget swallows the Enter.** Typing in the console opens
  completions. `typeInConsole` hides them before running the line.
- **The current line does not follow a live actor.** The napi adapter
  refreshes the view after each macrostep without taking the focus (napi
  ADR 0026), and VS Code moves the editor's current-line marker only when
  a frame is selected. A viewer clicks the top frame; so does the demo's
  `followActor()`, and then clicks back into the console.
- **Command IDs.** The Variables view is focused with
  `workbench.debug.action.focusVariablesView`, not
  `workbench.debug.variablesView.focus`. Search VS Code's keybindings
  (Preferences: Open Default Keyboard Shortcuts (JSON)) for an ID.
- **A live program does not follow the script.** The example server sends
  random events, so a scene that needs a state must put the actor there
  first, as the demo does before `goto active`.
- **Kill only what you started.** `record.sh` stops its own processes by
  PID. Never `pkill -f` a pattern: other sessions run their own servers
  and editors on the same machine.
