import test from 'node:test';
import assert from 'node:assert/strict';
import * as ta from '../../../bindings/web/node.mjs';
import { editorMachine, keyEvent, keyName, swapKeymap } from '../editor-machine.mjs';

function editor(t, input = {}, options = {}) {
  let now = 0;
  const system = ta.createSystem({ clock: () => now });
  t.after(() => system.stop());
  const actor = ta.createActor(editorMachine(ta, options), { system, input }).start();
  const snapshot = () => { system.flush(); const s = actor.getSnapshot(); assert.equal(s.error, undefined); return s; };
  return {
    actor, system, snapshot, c: () => snapshot().context,
    keys: (...keys) => { for (const key of keys) { actor.send(keyEvent(key)); snapshot(); } },
    tick: (ms) => { now += ms; system.flush(); snapshot(); },
  };
}

test('editor: counts multiply through operator + motion; aborted prefixes clear both counts', (t) => {
  const e = editor(t, { text: 'one two three four five six seven eight' });
  e.keys('3', 'd'); assert.equal(e.c().mode, 'operator'); assert.equal(e.c().count, '3');
  e.keys('2', 'w'); assert.equal(e.c().text, 'seven eight'); assert.equal(e.c().mode, 'normal');
  e.keys('u'); assert.equal(e.c().text, 'one two three four five six seven eight');
  e.keys('2', 'd', 'Escape', 'w'); assert.equal(e.c().cursor, 4);
  e.keys('d', 'z', 'w'); assert.equal(e.c().cursor, 8); assert.equal(e.c().mode, 'normal');
});

test('editor: zero is a motion or a count digit depending on its position; count timers renew', (t) => {
  const e = editor(t, { text: '012345678901234567890123456789', cursor: 5 });
  e.keys('2', 'd', '0'); assert.equal(e.c().text, '5678901234567890123456789');
  e.keys('u', 'd', '2'); e.tick(600); e.keys('0'); e.tick(600); e.keys('l');
  assert.equal(e.c().text, '0123456789');
  e.keys('u', '3', 'd'); e.tick(650); e.keys('l');
  assert.equal(e.c().cursor, 6); assert.equal(e.c().mode, 'normal');
});

test('editor: ci quote is nested, atomic undo across insert/normal; dot repeats the insertion', (t) => {
  const e = editor(t, { text: 'say "old" then "next"', cursor: 6 });
  e.keys('c', 'i'); assert.equal(e.c().prefix, 'c i');
  assert.match(JSON.stringify(e.snapshot().value.sequence), /key/);
  e.keys('"', 'N', 'E', 'W', 'Escape'); assert.equal(e.c().text, 'say "NEW" then "next"');
  assert.equal(e.c().undo.length, 1);
  e.actor.send({ type: 'SELECT', cursor: 16 }); e.keys('.');
  assert.equal(e.c().text, 'say "NEW" then "NEW"');
  e.keys('u'); assert.equal(e.c().text, 'say "NEW" then "next"');
  e.keys('u'); assert.equal(e.c().text, 'say "old" then "next"');
  e.keys('C-r'); assert.equal(e.c().text, 'say "NEW" then "next"');
});

test('editor: insert dot includes backspace and is one undo transaction', (t) => {
  const e = editor(t, { text: '--' });
  e.keys('i', 'a', 'b', 'Backspace', 'c', 'Escape', 'l', '.');
  assert.equal(e.c().text, 'ac-ac-');
  e.keys('u'); assert.equal(e.c().text, 'ac--');
});

test('editor: dot preserves arrow movement inside an insert session', (t) => {
  const e = editor(t, { text: '--' });
  e.keys('i', 'a', 'b', 'ArrowLeft', 'X', 'Escape', 'G', '.');
  assert.equal(e.c().text, 'aXb--aXb');
  e.keys('u'); assert.equal(e.c().text, 'aXb--');
});

test('editor: jj timeout boundary, mismatch, escape and blur never drop literal prefixes', (t) => {
  const e = editor(t, { text: '' });
  e.keys('i', 'j'); e.tick(649); assert.equal(e.c().text, ''); assert.equal(e.c().prefix, 'j');
  e.tick(1); assert.equal(e.c().text, 'j'); assert.equal(e.c().mode, 'insert');
  e.keys('j', 'j'); assert.equal(e.c().text, 'j'); assert.equal(e.c().mode, 'normal');
  e.keys('i', 'j', 'x'); assert.equal(e.c().text, 'jjx');
  e.keys('j', 'Escape'); assert.equal(e.c().text, 'jjxj'); assert.equal(e.c().mode, 'normal');
  e.keys('i', 'j'); e.actor.send({ type: 'BLUR' }); assert.equal(e.c().text, 'jjxjj');
  e.tick(1000); assert.equal(e.c().text, 'jjxjj');
});

test('editor: arbitrary-depth binding gets a fresh deadline at each trie level', (t) => {
  const e = editor(t, { text: '' }, { escape: 'j k l m n' });
  e.keys('i', 'j'); e.tick(600); e.keys('k'); e.tick(600); e.keys('l'); e.tick(600);
  assert.equal(e.c().prefix, 'j k l'); e.keys('m'); e.tick(600); e.keys('n');
  assert.equal(e.c().text, ''); assert.equal(e.c().mode, 'normal');
  e.keys('i', 'j', 'k', 'l'); e.tick(650); assert.equal(e.c().text, 'jkl');
});

test('editor: macros replay resolved commands as actor messages, including timed-out j', (t) => {
  const e = editor(t, { text: 'one two three' });
  e.keys('q', 'a', 'd', 'w', 'i', 'j'); e.tick(650); e.keys('Escape', 'q');
  assert.equal(e.c().text, 'jtwo three'); assert.ok(e.c().macros.a.length > 0);
  e.keys('@', 'a'); assert.equal(e.snapshot().value.macro, 'playing');
  for (let i = 0; i < 20; i++) e.tick(65);
  assert.equal(e.c().text, 'jjthree'); assert.equal(e.c().mode, 'normal');
  assert.equal(e.snapshot().value.macro, 'idle');
});

test('editor: Emacs numeric argument, arbitrary trie, mark, undo', (t) => {
  const e = editor(t, { text: 'abcdefghijk' }, { keymap: 'emacs' });
  e.keys('C-u', '4', 'C-f'); assert.equal(e.c().cursor, 4);
  e.keys('C-Space'); assert.deepEqual(e.snapshot().value.mode, { editing: { emacs: 'marked' } });
  e.keys('C-f', 'C-f', 'C-w'); assert.equal(e.c().text, 'abcdghijk');
  e.keys('C-x', 'u'); assert.equal(e.c().text, 'abcdefghijk');
  e.keys('C-x', 'r', 'r'); assert.equal(e.c().text, 'abcdghijk');
  e.keys('C-x', 'C-g', 'C-f'); assert.equal(e.c().cursor, 5);
});

test('editor: visual selection, undo and redo remain valid across mode changes', (t) => {
  const e = editor(t, { text: 'abcdef', cursor: 1 });
  e.keys('v', 'l', 'l'); assert.equal(e.c().mode, 'visual');
  e.keys('d'); assert.equal(e.c().text, 'aef'); assert.equal(e.c().mode, 'normal');
  e.keys('i', 'Z', 'Escape', 'u', 'u'); assert.equal(e.c().text, 'abcdef');
  e.keys('C-r', 'C-r'); assert.equal(e.c().text, 'aZef');
});

test('editor: migration mid-prefix retains address, text, cursor, history; cancels old timeout', async (t) => {
  const e = editor(t, { text: 'one two three' });
  e.keys('i', 'A', 'Escape', 'd');
  const address = e.actor.address, before = e.c();
  await swapKeymap(ta, e.system, e.actor, { keymap: 'emacs' });
  assert.equal(e.actor.address, address); assert.equal(e.c().text, before.text);
  assert.equal(e.c().cursor, before.cursor); assert.deepEqual(e.c().undo, before.undo);
  assert.equal(e.c().mode, 'emacs'); assert.equal(e.c().prefix, '');
  e.tick(700); e.keys('C-f'); assert.equal(e.c().cursor, before.cursor + 1);
  e.keys('C-x', 'u'); assert.equal(e.c().text, 'one two three');
});

test('editor: changing a binding flushes literal text, finalizes recording and cancels old timers', async (t) => {
  const e = editor(t, { text: '' });
  e.keys('q', 'a', 'i', 'j');
  await swapKeymap(ta, e.system, e.actor, { escape: 'j k l m' });
  assert.equal(e.c().text, 'j'); assert.equal(e.c().recording, '');
  e.keys('i', 'j'); e.tick(400); e.keys('k'); e.tick(400);
  assert.equal(e.c().prefix, 'j k'); assert.equal(e.c().text, 'j');
  e.keys('l', 'm', 'u'); assert.equal(e.c().text, '');
});

test('editor: save failure and stale success cannot overwrite newer edits; migration preserves in-flight save', async (t) => {
  const e = editor(t, { text: '' });
  e.keys('i', 'a'); e.actor.send({ type: 'FAIL_NEXT' }); e.actor.send({ type: 'SAVE' });
  e.keys('b'); e.tick(900); assert.equal(e.snapshot().value.save, 'failed');
  assert.equal(e.c().text, 'ab'); assert.equal(e.c().mode, 'insert');
  e.actor.send({ type: 'SAVE' }); e.keys('c');
  await swapKeymap(ta, e.system, e.actor, { keymap: 'emacs' });
  e.tick(900); assert.equal(e.c().savedText, 'ab'); assert.equal(e.c().text, 'abc');
  assert.equal(e.snapshot().value.save, 'dirty');
  e.tick(1800); e.tick(900); assert.equal(e.c().savedText, 'abc'); assert.equal(e.snapshot().value.save, 'clean');
});

test('editor: IME uses history to return to insert, commits once, and undo includes the composition', (t) => {
  const e = editor(t, { text: '' });
  e.keys('i'); e.actor.send({ type: 'COMPOSITION_START' });
  assert.equal(e.snapshot().value.mode, 'composing');
  e.actor.send({ type: 'COMPOSITION_END', text: '日本語' });
  assert.equal(e.c().text, '日本語'); assert.deepEqual(e.snapshot().value.mode, { editing: 'insert' });
  e.keys('Escape', 'u'); assert.equal(e.c().text, '');
});

test('editor: keys are hierarchical event names; the chart matches classes of keys by prefix, not by guards', (t) => {
  assert.equal(keyName('d'), 'key.char.letter.d');
  assert.equal(keyName('G'), 'key.char.letter.G');
  assert.equal(keyName('0'), 'key.char.digit.0');
  assert.equal(keyName('"'), 'key.char.symbol.quote');
  assert.equal(keyName('.'), 'key.char.symbol.period');
  assert.equal(keyName('é'), 'key.char.letter.ue9');
  assert.equal(keyName('🙂'), 'key.char.symbol.u1f642');
  assert.equal(keyName('C-x'), 'key.ctrl.x');
  assert.equal(keyName('C-Space'), 'key.ctrl.space');
  assert.equal(keyName('M-<'), 'key.meta.less');
  assert.equal(keyName('Escape'), 'key.special.escape');
  assert.equal(keyName('C'), 'key.char.letter.C');
  assert.equal(keyName('-'), 'key.char.symbol.minus');
  // stroke.char inserts any printable key, whatever its class
  const e = editor(t, { text: '' });
  e.keys('i', 'é', '🙂', '"', '.', '7', 'x');
  assert.equal(e.c().text, 'é🙂".7x');
  const scxml = e.actor.logic.toSCXML();
  for (const d of ['"stroke.char"', '"stroke.char.digit"', '"stroke.char.digit.0"', '"stroke.char.letter.d"', '"stroke"', '"key"'])
    assert.ok(scxml.includes(`event=${d}`), `a transition on ${d}`);
  assert.doesNotMatch(scxml, /cond="[^"]*event\.key/, 'no guard inspects the key');
});
