// Gemini's generateContent over REST, with the copywriter's functions,
// thinking, and audio clips as inline data. The key goes in the `x-goog-api-key`
// header of each request and nowhere else.

import { BASE, KEY_REFUSED } from './models.mjs';

/** The functions the model may call (model-tools.xml, adapted). */
export const FUNCTIONS = Object.freeze([
  {
    name: 'propose_rewrite',
    description: 'Propose a plain-text replacement for a piece of the page\'s text, for the user to preview and keep or discard. It does not change the page. Never HTML, Markdown or code.',
    parameters: {
      type: 'object',
      properties: {
        original_text: {
          type: 'string',
          description: 'The exact text on the page to replace, copied verbatim. Omit it when the rewrite is of the selected text.',
        },
        replacement: { type: 'string', description: 'The proposed replacement, plain text.' },
        rationale: { type: 'string', description: 'One sentence: why the rewrite is better. When audiences are active, which need of which one it serves, by name.' },
      },
      required: ['replacement', 'rationale'],
    },
  },
  {
    name: 'set_note',
    description: 'Record, in one sentence, what is wrong with the selected text, when the user has not said it yet.',
    parameters: {
      type: 'object',
      properties: { note: { type: 'string', description: 'The problem, in one sentence.' } },
      required: ['note'],
    },
  },
  {
    name: 'define_audience',
    description: 'Create or update an audience: a kind of reader of the page (not a writing voice). Call it when the reviewer describes who reads, or asks who the page is for. An audience of the same name is updated.',
    parameters: {
      type: 'object',
      properties: {
        name: { type: 'string', description: 'A short name for the reader, such as "Skeptical senior backend engineer".' },
        description: { type: 'string', description: 'Who they are, in one or two sentences.' },
        knows: { type: 'string', description: 'What they already know.' },
        wants: { type: 'string', description: 'The information they want from the page.' },
        tasks: { type: 'string', description: 'The tasks they came to the page to do.' },
        objections: { type: 'string', description: 'Their doubts and objections.' },
        use: { type: 'boolean', description: 'True to make it one of the active audiences now.' },
      },
      required: ['name', 'description'],
    },
  },
  {
    name: 'use_audiences',
    description: 'Set the active audiences, by their names: the rewrites are then written for them. An empty list writes for no audience in particular.',
    parameters: {
      type: 'object',
      properties: { names: { type: 'array', items: { type: 'string' }, description: 'The audiences\' names.' } },
      required: ['names'],
    },
  },
]);

/**
 * Google's built-in tools a reviewer can turn on, by setting:
 * search (Google Search grounding), url (URL context: the model reads the
 * pages a message names) and code (code execution). Defaults: url only.
 * With our functions in the same request they need
 * `toolConfig.includeServerSideToolInvocations` (checked 2026-11-23 on
 * Gemini 4 Hydrogen and 3.8 Flash: without it, 400).
 */
export const BUILT_IN_TOOLS = Object.freeze({ search: 'googleSearch', url: 'urlContext', code: 'codeExecution' });
export const DEFAULT_TOOLS = Object.freeze({ search: false, url: true, code: false });

/** The thinking levels a reviewer can pick; the default is 'high'. */
export const THINKING_LEVELS = Object.freeze(['low', 'medium', 'high', 'xhigh']);
export const DEFAULT_THINKING = 'high';

/** The most tokens an answer may take, thoughts included. */
export const MAX_OUTPUT_TOKENS = 65536;

// Models that take thinkingLevel 'xhigh' (generateContent, 2026-11-23:
// Gemini 4 Hydrogen and Neon; Carbon, Argon and every 3.x answer 400).
const XHIGH = /^gemini-4-(hydrogen|neon)(\b|-)/;
// gemini-2.5 models take a token budget, not a level.
const BUDGETS = Object.freeze({ low: 1024, medium: 8192, high: 24576, xhigh: 24576 });

/**
 * The generationConfig.thinkingConfig for a model and a level: a
 * thinkingLevel for Gemini 3 and 4 ('xhigh' only where the model takes it,
 * 'high' elsewhere), a thinkingBudget for gemini-2.5.
 */
export function thinkingConfig(model, level = DEFAULT_THINKING, { includeThoughts = true } = {}) {
  const wanted = THINKING_LEVELS.includes(level) ? level : DEFAULT_THINKING;
  const config = /^gemini-2\.5/.test(model)
    ? { thinkingBudget: BUDGETS[wanted] }
    : { thinkingLevel: wanted === 'xhigh' && !XHIGH.test(model) ? 'high' : wanted };
  if (includeThoughts) config.includeThoughts = true;
  return config;
}

/** A refusal or failure of the Gemini API, with a message for the user. */
export class GeminiError extends Error {
  constructor(message, { status } = {}) {
    super(message);
    this.name = 'GeminiError';
    this.status = status;
  }
}

// What the user reads for each refusal, then Google's own message.
function explain(status, detail, reason = '') {
  const said = detail ? ` Google said: ${detail}` : '';
  if (/API_KEY_INVALID/.test(reason) || /API key not valid/i.test(detail)) return `${KEY_REFUSED} Check it in Settings.`;
  if (status === 400) return `Gemini refused the request (400): check the model in Settings.${said}`;
  if (status === 401 || status === 403) return `Gemini refused the key (${status}): check it in Settings.${said}`;
  if (status === 404) return `Gemini does not know this model (404): pick another in Settings.${said}`;
  if (status === 429) return `Gemini's rate limit or quota is reached (429): wait a minute and try again.${said}`;
  if (status >= 500) return `Gemini failed (${status}): try again.${said}`;
  return `Gemini answered ${status}.${said}`;
}

/** Bytes as base64, in a page and in Node.js. */
export function base64(bytes) {
  let text = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return btoa(text);
}

/**
 * The conversation as Gemini's `contents`: each message a turn (`model`
 * or `user`), turns of one role merged, and a clip ({mimeType, bytes})
 * added to the last user turn as inline data.
 */
export function contents(messages, { prompt, clip } = {}) {
  const turns = [];
  const add = (role, part) => {
    const last = turns[turns.length - 1];
    if (last && last.role === role) last.parts.push(part);
    else turns.push({ role, parts: [part] });
  };
  for (const m of messages) add(m.role === 'model' ? 'model' : 'user', { text: m.text });
  if (prompt) add('user', { text: prompt });
  if (clip) add('user', { inlineData: { mimeType: clip.mimeType, data: base64(clip.bytes) } });
  if (turns.length === 0 || turns[0].role !== 'user') turns.unshift({ role: 'user', parts: [{ text: '(start)' }] });
  return turns;
}

/**
 * The text, the function calls and the thought summary of a
 * generateContent response. A part marked `thought` is never the answer.
 * An answer cut at the output limit (`MAX_TOKENS`) keeps what it has; one
 * cut before it said anything is a GeminiError.
 */
export function parse(body) {
  const candidate = (body.candidates || [])[0];
  if (!candidate) {
    const reason = body.promptFeedback?.blockReason;
    throw new GeminiError(reason ? `Gemini blocked the request (${reason}).` : 'Gemini gave no answer.');
  }
  const texts = [];
  const thoughts = [];
  const calls = [];
  for (const part of candidate.content?.parts || []) {
    if (part.thought) {
      if (typeof part.text === 'string') thoughts.push(part.text);
      continue;
    }
    if (typeof part.text === 'string') texts.push(part.text);
    if (part.functionCall) calls.push({ name: part.functionCall.name, args: part.functionCall.args || {}, ...(part.functionCall.id ? { id: part.functionCall.id } : {}) });
  }
  const text = texts.join('').trim();
  // the web pages the answer was grounded on (Google Search, URL context), once each
  const sources = [];
  for (const chunk of candidate.groundingMetadata?.groundingChunks || []) {
    const web = chunk.web || chunk.retrievedContext;
    if (web?.uri && !sources.some((x) => x.uri === web.uri)) sources.push({ title: String(web.title || web.uri), uri: String(web.uri) });
  }
  const cut = candidate.finishReason === 'MAX_TOKENS';
  if (cut && !text && calls.length === 0) {
    throw new GeminiError('Gemini used its whole output budget thinking and gave no answer: try again, or a lower thinking level in Settings.');
  }
  // the parts as given (thought signatures included), to send back with the calls' results
  return {
    text, calls, parts: candidate.content?.parts || [],
    thoughts: thoughts.join('\n\n').trim(), sources, ...(cut ? { cut: true } : {}),
  };
}

/**
 * The turns that answer a reply of function calls: the model's own turn
 * as it came, then each call's result. Sent again with the same request,
 * they ask the model for the words it left out.
 */
export function callResults({ parts, calls }) {
  return [
    { role: 'model', parts },
    { role: 'user', parts: calls.map((c) => ({ functionResponse: { name: c.name, ...(c.id ? { id: c.id } : {}), response: { result: CALL_RESULTS[c.name] || 'Done.' } } })) },
  ];
}

// What each call did, as the model reads it in a functionResponse.
const CALL_RESULTS = Object.freeze({
  propose_rewrite: 'Shown to the reviewer as a proposal to preview.',
  set_note: 'Saved as the note.',
  define_audience: 'Saved; the reviewer sees it in Settings.',
  use_audiences: 'They are the active audiences now.',
});

/**
 * One generateContent request: `{text, calls, parts, thoughts, cut?}`, or
 * a GeminiError whose message says what to do. `thinking` is a level of
 * THINKING_LEVELS, mapped per model by thinkingConfig.
 */
export async function generate({
  apiKey, model, system, contents, fetch = globalThis.fetch,
  thinking = DEFAULT_THINKING, includeThoughts = true, tools = FUNCTIONS, builtIn = {},
}) {
  if (!apiKey) throw new GeminiError('No Gemini key: add yours in Settings.');
  const servers = Object.entries(BUILT_IN_TOOLS).filter(([name]) => builtIn[name]).map(([, tool]) => ({ [tool]: {} }));
  const all = [...(tools.length ? [{ functionDeclarations: tools }] : []), ...servers];
  const request = {
    systemInstruction: { parts: [{ text: system }] },
    contents,
    ...(all.length ? { tools: all } : {}),
    ...(servers.length && tools.length ? { toolConfig: { includeServerSideToolInvocations: true } } : {}),
    generationConfig: { maxOutputTokens: MAX_OUTPUT_TOKENS, thinkingConfig: thinkingConfig(model, thinking, { includeThoughts }) },
  };
  let response;
  try {
    response = await fetch(`${BASE}/models/${encodeURIComponent(model)}:generateContent`, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-goog-api-key': apiKey },
      body: JSON.stringify(request),
    });
  } catch (error) {
    throw new GeminiError(`Gemini could not be reached: ${error.message}`);
  }
  if (!response.ok) {
    let detail = '';
    let reason = '';
    try {
      const error = (await response.json()).error || {};
      detail = error.message || '';
      reason = [error.status, ...(error.details || []).map((d) => d.reason)].filter(Boolean).join(' ');
    } catch { /* not JSON */ }
    // A model the table does not know refuses the level: once more at 'high'.
    if (response.status === 400 && /thinking level/i.test(detail) && thinking !== 'high') {
      return generate({ apiKey, model, system, contents, fetch, thinking: 'high', includeThoughts, tools, builtIn });
    }
    throw new GeminiError(explain(response.status, detail, reason), { status: response.status });
  }
  return parse(await response.json());
}

/**
 * What the copywriter's processors need from a reply: the text, the
 * rewrites (`propose_rewrite`'s arguments), a note (`set_note`), the
 * audience calls (`define_audience` as `{action: 'define', name,
 * description, knows?, wants?, tasks?, objections?, use}`, `use_audiences`
 * as `{action: 'use', names}`) and the thought summary. Calls of other
 * names, or with the wrong types, are dropped.
 */
export function reading({ text, calls, thoughts = '', sources = [] }) {
  const rewrites = [];
  const audiences = [];
  let note = null;
  const clean = (value, max = 600) => (typeof value === 'string' ? value.trim().slice(0, max) : '');
  for (const call of calls) {
    const a = call.args;
    if (call.name === 'propose_rewrite' && typeof a.replacement === 'string' && a.replacement.trim()) {
      const r = { replacement: a.replacement.trim(), rationale: typeof a.rationale === 'string' ? a.rationale : '' };
      if (typeof a.original_text === 'string' && a.original_text.trim()) r.original_text = a.original_text.trim();
      rewrites.push(r);
    } else if (call.name === 'set_note' && typeof a.note === 'string') {
      note = a.note.trim();
    } else if (call.name === 'define_audience' && clean(a.name, 80) && clean(a.description)) {
      const audience = { action: 'define', name: clean(a.name, 80), description: clean(a.description), use: a.use === true };
      for (const field of ['knows', 'wants', 'tasks', 'objections']) if (clean(a[field])) audience[field] = clean(a[field]);
      audiences.push(audience);
    } else if (call.name === 'use_audiences' && Array.isArray(a.names)) {
      audiences.push({ action: 'use', names: a.names.map((n) => clean(n, 80)).filter(Boolean) });
    }
  }
  return { text, rewrites, note, audiences, thoughts, sources };
}

/** What the transcription asks of the model. */
export const TRANSCRIBE = 'Transcribe the speech in this audio verbatim, in the language spoken. Answer with the transcript only: no commentary, no quotation marks, no labels. If nothing is said, answer with nothing.';

/**
 * A clip ({mimeType, bytes}) written down: one generateContent request
 * with the audio inline, no tools, low thinking. Resolves to the text
 * ('' when nothing was heard).
 */
export async function transcribe({ apiKey, model, clip, fetch = globalThis.fetch }) {
  const result = await generate({
    apiKey, model, fetch, system: TRANSCRIBE, tools: [], thinking: 'low', includeThoughts: false,
    contents: [{ role: 'user', parts: [{ text: 'Transcribe this.' }, { inlineData: { mimeType: clip.mimeType, data: base64(clip.bytes) } }] }],
  });
  return result.text;
}

/** How long to wait before each retry of a request Google turned away for
 * load (429, 500, 503): spikes are usually brief. */
export const RETRY_DELAYS = Object.freeze([1500, 4000, 9000]);

/**
 * `fetch`, retried after each of `delays` when Google answers 429, 500 or
 * 503 (a Retry-After of at most 20 s is honoured); the last answer is
 * returned as it is. `sleep` is injectable for tests.
 */
export function retrying(fetch, delays = RETRY_DELAYS, sleep = (ms) => new Promise((r) => setTimeout(r, ms))) {
  return async (url, options) => {
    for (let attempt = 0; ; attempt++) {
      const response = await fetch(url, options);
      if (![429, 500, 503].includes(response.status) || attempt >= delays.length) return response;
      const after = Number(response.headers?.get?.('retry-after'));
      await sleep(after > 0 && after <= 20 ? after * 1000 : delays[attempt]);
    }
  };
}
