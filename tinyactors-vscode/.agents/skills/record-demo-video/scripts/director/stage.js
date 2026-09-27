'use strict';

// What a director does to VS Code on camera. It runs in the extension host
// (the director is VS Code's --extensionTestsPath), so it has the whole
// `vscode` API, and it drives the X server with xdotool where only a real
// pointer does the job. See ../../SKILL.md.

const vscode = require('vscode');
const { execFileSync } = require('node:child_process');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const WIDTH = Number(process.env.DEMO_WIDTH ?? 1600);
const HEIGHT = Number(process.env.DEMO_HEIGHT ?? 900);

const xdotool = (...args) => execFileSync('xdotool', args.map(String)).toString();

// Xvfb has no window manager, so VS Code opens at its default size in the
// middle of the display: move it to the corner and make it fill the screen.
function fillScreen() {
  for (const id of xdotool('search', '--onlyvisible', '--class', 'code').trim().split('\n')) {
    xdotool('windowmove', id, 0, 0);
    xdotool('windowsize', id, WIDTH, HEIGHT);
  }
}

// Clicks at a point of the display, as a person would. Find a point by
// taking a screenshot of the layout (SKILL.md, "Finding where to click").
async function click(x, y) {
  xdotool('mousemove', x, y);
  await sleep(250);
  xdotool('click', 1);
  await sleep(350);
}

// A caption: a notification, cleared after `ms` so they do not pile up.
async function caption(text, ms = 3500) {
  vscode.window.showInformationMessage(text);
  await sleep(ms);
  await vscode.commands.executeCommand('notifications.clearAll');
}

// Types one Debug Console line and runs it, one character at a time so it
// reads on video. The console is focused before typing and again before
// accepting: anything that moves the focus in between (a stop, an editor
// shown) would send the rest of the line to wherever the focus went.
async function typeInConsole(text) {
  await vscode.commands.executeCommand('workbench.debug.action.focusRepl');
  await sleep(250);
  await vscode.commands.executeCommand('editor.action.selectAll');
  await vscode.commands.executeCommand('deleteLeft');
  for (const character of text) {
    await vscode.commands.executeCommand('type', { text: character });
    await sleep(60);
  }
  await sleep(450);
  await vscode.commands.executeCommand('hideSuggestWidget');
  await vscode.commands.executeCommand('workbench.debug.action.focusRepl');
  await vscode.commands.executeCommand('repl.action.acceptInput');
}

module.exports = { sleep, fillScreen, click, caption, typeInConsole };
