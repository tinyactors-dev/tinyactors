// The decide processor's real client, Jev through TypeSafe's API, against
// a recorded fetch: what it sends (the endpoint, the key, the chart's
// request with the model) and what it makes of each answer. The scenario
// tests use the stand-in; this is the one test of the wire.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { Jev } from '../lib/processors/jev.mjs';

const request = {
  state: { report: { title: 'examples fail before the build' }, papercuts: [{ id: 'p1', title: 'examples need the addon' }] },
  questions: { duplicate: { type: 'choice', instructions: 'Same problem?', criteria: { new: 'another', p1: 'issue #1' } } },
};

// A fetch that records its call and answers with `status` and `body`.
function recorded(status, body) {
  const calls = [];
  const fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: status >= 200 && status < 300, status, json: async () => body };
  };
  return { calls, fetch };
}

describe('Jev through TypeSafe', () => {
  it('posts the chart\'s request with the model and the key, and answers decided', async () => {
    const answers = { duplicate: { type: 'choice', choice: 'p1', confidence: 0.98, probabilities: { new: 0.02, p1: 0.98 } } };
    const { calls, fetch } = recorded(200, { model: 'jev-1.14.0', answers, usage: { input_tokens: 391, output_tokens: 33 } });
    const result = await new Jev({ key: 'the-key', fetch }).processor('decide', request);
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'https://api.typesafe.ai/v1/systemone');
    assert.equal(calls[0].options.method, 'POST');
    assert.equal(calls[0].options.headers.authorization, 'Bearer the-key');
    assert.deepEqual(JSON.parse(calls[0].options.body), { ...request, model: 'jev-latest' });
    assert.deepEqual(result, { event: 'decided', data: { model: 'jev-1.14.0', answers } });
  });

  it('answers decide.unavailable without a key, sending nothing', async () => {
    const { calls, fetch } = recorded(200, {});
    const result = await new Jev({ fetch }).processor('decide', request);
    assert.equal(result.event, 'decide.unavailable');
    assert.equal(calls.length, 0);
  });

  it('answers decide.unavailable when TypeSafe refuses or cannot be reached', async () => {
    const refused = await new Jev({ key: 'k', fetch: recorded(401, { detail: 'Unauthorized' }).fetch }).processor('decide', request);
    assert.deepEqual(refused, { event: 'decide.unavailable', data: { status: 401 } });
    const down = await new Jev({ key: 'k', fetch: async () => { throw new Error('connect ECONNREFUSED'); } }).processor('decide', request);
    assert.equal(down.event, 'decide.unavailable');
    assert.match(down.data.reason, /ECONNREFUSED/);
  });

  it('refuses another event', async () => {
    assert.equal(await new Jev({ key: 'k', fetch: recorded(200, {}).fetch }).processor('compare', request), null);
  });
});
