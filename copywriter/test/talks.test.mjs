// Talking to an audience: a conversation of the page chat held with an
// audience (charts/conversation.scxml in its reader mode) against a fake
// page and a fake Gemini. The model speaks as the reader, refines the
// audience when corrected (update_audience) and notes what the page misses
// (note_gap); the editor opens such a conversation with talk_to.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup, fakePage, fakeFetch, geminiReply, find } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';

const TEXT = 'Actors as a library. Four million actors fit in 1.5 GB. Install it with npm. Licensed under MIT.';

const LEAD = {
  id: 'lead', name: 'Tech lead', description: 'Approves dependencies.',
  knows: 'SQLite and embedded libraries.',
  wants: 'The license',
  objections: '"Who maintains it?"',
};

async function withLead(options = {}) {
  const env = await setup({ text: TEXT, ...options });
  await env.reviewer.dispatch({ type: 'settings.save', audience: LEAD });
  return env;
}

const chat = (reviewer) => reviewer.getState().chat;
const system = (request) => request.body.systemInstruction.parts[0].text;
const functions = (request) => (request.body.tools || []).flatMap((t) => (t.functionDeclarations || []).map((f) => f.name));

test('talk: a conversation with an audience; the model is asked to speak as that reader, with the reader\'s tools only', async () => {
  const fetch = fakeFetch(geminiReply('The page doesn\'t tell me who maintains it, so I would close the tab.'));
  const { reviewer } = await withLead({ fetch });
  const editor = chat(reviewer).id;
  const id = await reviewer.dispatch({ type: 'talk', audienceID: 'lead' });
  assert.notEqual(id, editor, 'the editor\'s empty conversation is left for a new one');
  assert.equal(chat(reviewer).id, id);
  assert.equal(chat(reviewer).audienceID, 'lead');
  assert.deepEqual(chat(reviewer).messages, []);
  assert.equal(await reviewer.dispatch({ type: 'talk', audienceID: 'lead' }), id, 'an empty one with the same audience is kept');

  await reviewer.dispatch({ type: 'chat', text: 'What would make you close this tab?' });
  assert.equal(fetch.requests.length, 1);
  const request = fetch.requests[0];
  const text = system(request);
  assert.match(text, /You:\n- Tech lead: Approves dependencies\.\n {2}Knows: SQLite and embedded libraries\./);
  assert.match(text, /Speak in the first person as this reader/);
  assert.match(text, /Never invent facts about the product/);
  assert.match(text, /The page doesn\\?'t tell me whether/);
  assert.match(text, /Licensed under MIT\./, 'the page text');
  assert.doesNotMatch(text, /You help a reviewer improve the copy/, 'not the editor\'s instruction');
  assert.deepEqual(functions(request), ['update_audience', 'note_gap'], 'no propose_rewrite, no talk_to');
  assert.equal(request.body.tools.length, 1, 'no built-in tool: the reader answers from the page');
  assert.deepEqual(request.body.contents.at(-1), { role: 'user', parts: [{ text: 'What would make you close this tab?' }] });

  const reply = chat(reviewer).messages.at(-1);
  assert.equal(reply.role, 'model');
  assert.match(reply.text, /^The page doesn't tell me who maintains it/);
  assert.equal(chat(reviewer).busy, false);
  reviewer.close();
});

test('talk_to from the editor: a conversation with that audience opens; the editor\'s message links to it', async () => {
  const fetch = fakeFetch(
    geminiReply('Here is the tech lead.', [['talk_to', { audience: 'tech lead' }], ['talk_to', { audience: 'Nobody' }]]),
    geminiReply('I approve dependencies. Ask me anything.'),
  );
  const { reviewer } = await withLead({ fetch });
  const editor = chat(reviewer).id;
  await reviewer.dispatch({ type: 'chat', text: 'Let me talk to the tech lead.' });
  assert.deepEqual(functions(fetch.requests[0]), ['propose_rewrite', 'set_note', 'define_audience', 'read_as', 'talk_to', 'use_audiences']);
  assert.match(system(fetch.requests[0]), /call talk_to with its name/);

  const state = chat(reviewer);
  assert.notEqual(state.id, editor, 'the audience\'s conversation is the open one');
  assert.equal(state.audienceID, 'lead');
  assert.deepEqual(state.messages, []);
  const listed = state.conversations.find((c) => c.id === editor);
  assert.equal(listed.audienceID, null, 'the editor\'s conversation is listed');

  // back to the editor: its message names the conversation it opened
  await reviewer.dispatch({ type: 'chat.open', id: editor });
  const said = chat(reviewer).messages.at(-1);
  assert.match(said.text, /^Here is the tech lead\. No audience named “Nobody”\./);
  assert.deepEqual(said.audiences, [{ name: 'Tech lead', action: 'talk', id: 'lead', chat: state.id }]);
  // and the chip opens it again, though nothing was said in it yet
  await reviewer.dispatch({ type: 'chat.open', id: state.id });
  assert.equal(chat(reviewer).audienceID, 'lead');
  await reviewer.dispatch({ type: 'chat', text: 'Who are you?' });
  assert.deepEqual(functions(fetch.requests[1]), ['update_audience', 'note_gap'], 'the reader\'s request');
  // a new conversation is the editor's again
  await reviewer.dispatch({ type: 'chat.new' });
  assert.equal(chat(reviewer).audienceID, null);
  reviewer.close();
});

test('update_audience: a correction refines the audience, the message shows what changed, and a later reading uses it', async () => {
  const fetch = fakeFetch(
    geminiReply('Right: with Erlang behind me, "actors as a library" reads as a claim I can check.', [
      ['update_audience', { name: 'Tech lead', knows: 'SQLite and embedded libraries; Erlang and OTP well.', wants: 'The license' }],
    ]),
    geminiReply('', [['report_coverage', { item: 1, status: 'answered', explanation: 'MIT.', answer_text: 'Licensed under MIT.' }]]),
  );
  const { reviewer, store } = await withLead({ fetch });
  await reviewer.dispatch({ type: 'talk', audienceID: 'lead' });
  await reviewer.dispatch({ type: 'chat', text: 'You\'d actually know Erlang well.' });

  const reply = chat(reviewer).messages.at(-1);
  assert.match(reply.text, /^Right: with Erlang behind me/);
  assert.deepEqual(reply.audiences, [{
    name: 'Tech lead', action: 'update', id: 'lead',
    changes: [{ field: 'knows', before: 'SQLite and embedded libraries.', after: 'SQLite and embedded libraries; Erlang and OTP well.' }],
  }], 'only what changed: wants was given as it was');
  const lead = reviewer.getState().settings.audiences.find((a) => a.id === 'lead');
  assert.equal(lead.knows, 'SQLite and embedded libraries; Erlang and OTP well.');
  assert.equal(lead.name, 'Tech lead');
  assert.equal((await store.audiences()).find((a) => a.id === 'lead').knows, lead.knows, 'saved');

  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  assert.match(system(fetch.requests[1]), /Knows: SQLite and embedded libraries; Erlang and OTP well\./, 'the reading reads as the refined audience');
  reviewer.close();
});

test('the reader sees its last reading of the page; a reply of calls only is followed up for its words', async () => {
  const fetch = fakeFetch(
    geminiReply('', [['report_coverage', { item: 1, status: 'answered', explanation: 'MIT, at the end.', answer_text: 'Licensed under MIT.' }],
      ['report_coverage', { item: 2, status: 'missing', explanation: 'No maintainer is named.' }]]),
    geminiReply('', [['note_gap', { item: 'Name the maintainer', explanation: 'I will not depend on a project with no owner.' }]]),
    geminiReply('Noted: I need to know who maintains it.'),
  );
  const { reviewer } = await withLead({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  await reviewer.dispatch({ type: 'talk', audienceID: 'lead' });
  await reviewer.dispatch({ type: 'chat', text: 'What is missing for you?' });

  const text = system(fetch.requests[1]);
  assert.match(text, /Your last reading of this page \(\d{4}-\d\d-\d\d\):\n- \[answered\] Wants: The license \(MIT, at the end\.\)\n- \[missing\] Objections: "Who maintains it\?" \(No maintainer is named\.\)/);
  // calls only: the call's result goes back, and the second answer is the words
  assert.equal(fetch.requests.length, 3);
  const followUp = fetch.requests[2].body.contents.slice(-2);
  assert.equal(followUp[0].role, 'model');
  assert.equal(followUp[1].parts[0].functionResponse.name, 'note_gap');
  const reply = chat(reviewer).messages.at(-1);
  assert.equal(reply.text, 'Noted: I need to know who maintains it.');
  assert.equal(reply.annotations.length, 1);
  reviewer.close();
});

test('note_gap: what the reader misses becomes a page-level open item of kind missing, the audience\'s finding', async () => {
  const fetch = fakeFetch(geminiReply('I still cannot tell who maintains it.', [
    ['note_gap', { item: 'Name who maintains it', explanation: 'I will not depend on a project with no owner.' }],
    ['note_gap', { item: '  ' }],
  ]));
  const { reviewer } = await withLead({ fetch });
  await reviewer.dispatch({ type: 'talk', audienceID: 'lead' });
  await reviewer.dispatch({ type: 'chat', text: 'Would you approve it?' });
  const reply = chat(reviewer).messages.at(-1);
  assert.equal(reply.annotations?.length, 1, 'an empty item is dropped');
  const a = find(reviewer, reply.annotations[0]);
  assert.equal(a.pageLevel, true);
  assert.equal(a.source, 'audience');
  assert.equal(a.state, 'draft');
  assert.equal(a.original, 'Name who maintains it');
  assert.deepEqual(a.finding, { audienceID: 'lead', kind: 'missing', explanation: 'I will not depend on a project with no owner.' });
  assert.deepEqual(a.audiences, ['Tech lead']);
  assert.equal(reviewer.getState().totals.open, 1);
  reviewer.close();
});

test('a conversation with an audience comes back with the page, still the reader\'s; a deleted audience is said', async () => {
  const store = createMemoryStore();
  const first = await withLead({ store, fetch: fakeFetch(geminiReply('I approve dependencies.')) });
  const id = await first.reviewer.dispatch({ type: 'talk', audienceID: 'lead' });
  await first.reviewer.dispatch({ type: 'chat', text: 'Who are you?' });
  first.reviewer.close();
  assert.equal((await store.conversations('https://www.tinyactors.dev/')).find((c) => c.id === id).audienceID, 'lead');

  // the page opens with a key and an audience: it is asked which audiences read it, once
  const fetch = fakeFetch(geminiReply('', [['page_audiences', { names: [] }]]), geminiReply('Still the tech lead.'));
  const { reviewer } = await setup({ store, fetch, page: fakePage({ text: TEXT }) });
  assert.equal(chat(reviewer).id, id, 'the newest conversation opens');
  assert.equal(chat(reviewer).audienceID, 'lead');
  assert.equal(chat(reviewer).conversations[0].audienceID, 'lead');
  await reviewer.dispatch({ type: 'chat', text: 'And now?' });
  assert.deepEqual(functions(fetch.requests[0]), ['page_audiences']);
  assert.deepEqual(functions(fetch.requests[1]), ['update_audience', 'note_gap']);
  await reviewer.dispatch({ type: 'settings.deleteAudience', id: 'lead' });
  await reviewer.dispatch({ type: 'chat', text: 'Still there?' });
  assert.equal(chat(reviewer).error, 'That audience was deleted.');
  await assert.rejects(reviewer.dispatch({ type: 'talk', audienceID: 'lead' }), /No such audience/);
  reviewer.close();
});
