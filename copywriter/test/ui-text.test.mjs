// The panel's text helpers: a checklist item as shown, and an audience
// update's diff word by word.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { displayItem, wordDiff } from '../src/ui/panel.mjs';

test('a checklist item starts with a capital when it starts with a word, never with code', () => {
  assert.equal(displayItem('check the license'), 'Check the license');
  assert.equal(displayItem('well-known limits'), 'Well-known limits');
  assert.equal(displayItem('npm install, run the first chart'), 'npm install, run the first chart');
  assert.equal(displayItem('pip install tinyactors'), 'pip install tinyactors');
  assert.equal(displayItem('`spawn` an actor'), '`spawn` an actor');
  assert.equal(displayItem('ta_host_run in a loop'), 'ta_host_run in a loop');
  assert.equal(displayItem('package.json first'), 'package.json first');
  assert.equal(displayItem('"Who maintains it?"'), '"Who maintains it?"');
  assert.equal(displayItem(''), '');
});

test('an update\'s diff strikes only the words removed and marks the words added', () => {
  assert.deepEqual(wordDiff('Knows SQLite and embedded libraries.', 'Knows SQLite, Erlang and libraries.'), [
    { op: '=', text: 'Knows SQLite' }, { op: '+', text: ', Erlang ' }, { op: '=', text: 'and ' }, { op: '-', text: 'embedded ' }, { op: '=', text: 'libraries.' },
  ]);
  assert.deepEqual(wordDiff('', 'New text.'), [{ op: '+', text: 'New text.' }]);
  assert.deepEqual(wordDiff('Same.', 'Same.'), [{ op: '=', text: 'Same.' }]);
});
