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
        rationale: { type: 'string', description: 'One sentence: why it serves the style guide and the reader.' },
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
    name: 'define_persona',
    description: 'Create or update a persona: a named voice or audience whose instructions guide every rewrite while it is active. Call it when the reviewer describes an audience, a reader or a voice. A persona of the same name is updated.',
    parameters: {
      type: 'object',
      properties: {
        name: { type: 'string', description: 'A short name, such as "Skeptical CTO".' },
        instructions: { type: 'string', description: 'The style guide for this persona, as instructions to a copy editor: who reads, what they need, the voice, what to avoid.' },
        use: { type: 'boolean', description: 'True to make it the active persona now.' },
      },
      required: ['name', 'instructions'],
    },
  },
  {
    name: 'use_persona',
    description: 'Make a saved persona the active one, by its name.',
    parameters: {
      type: 'object',
      properties: { name: { type: 'string', description: 'The persona\'s name.' } },
      required: ['name'],
    },
  },
]);

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
    if (part.functionCall) calls.push({ name: part.functionCall.name, args: part.functionCall.args || {} });
  }
  const text = texts.join('').trim();
  const cut = candidate.finishReason === 'MAX_TOKENS';
  if (cut && !text && calls.length === 0) {
    throw new GeminiError('Gemini used its whole output budget thinking and gave no answer: try again, or a lower thinking level in Settings.');
  }
  // the parts as given (thought signatures included), to send back with the calls' results
  return {
    text, calls, parts: candidate.content?.parts || [],
    thoughts: thoughts.join('\n\n').trim(), ...(cut ? { cut: true } : {}),
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
    { role: 'user', parts: calls.map((c) => ({ functionResponse: { name: c.name, response: { result: CALL_RESULTS[c.name] || 'Done.' } } })) },
  ];
}

// What each call did, as the model reads it in a functionResponse.
const CALL_RESULTS = Object.freeze({
  propose_rewrite: 'Shown to the reviewer as a proposal to preview.',
  set_note: 'Saved as the note.',
  define_persona: 'Saved; the reviewer sees it in Settings.',
  use_persona: 'It is the active persona now.',
});

/**
 * One generateContent request: `{text, calls, parts, thoughts, cut?}`, or
 * a GeminiError whose message says what to do. `thinking` is a level of
 * THINKING_LEVELS, mapped per model by thinkingConfig.
 */
export async function generate({
  apiKey, model, system, contents, fetch = globalThis.fetch,
  thinking = DEFAULT_THINKING, includeThoughts = true, tools = FUNCTIONS,
}) {
  if (!apiKey) throw new GeminiError('No Gemini key: add yours in Settings.');
  const request = {
    systemInstruction: { parts: [{ text: system }] },
    contents,
    ...(tools.length ? { tools: [{ functionDeclarations: tools }] } : {}),
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
      return generate({ apiKey, model, system, contents, fetch, thinking: 'high', includeThoughts, tools });
    }
    throw new GeminiError(explain(response.status, detail, reason), { status: response.status });
  }
  return parse(await response.json());
}

/**
 * What the copywriter's processors need from a reply: the text, the
 * rewrites (`propose_rewrite`'s arguments), a note (`set_note`), the
 * persona calls (`define_persona`, `use_persona`, as `{action: 'define'
 * | 'use', name, instructions?, use?}`) and the thought summary. Calls of
 * other names, or with the wrong types, are dropped.
 */
export function reading({ text, calls, thoughts = '' }) {
  const rewrites = [];
  const personas = [];
  let note = null;
  const name = (a) => (typeof a.name === 'string' ? a.name.trim().slice(0, 80) : '');
  for (const call of calls) {
    const a = call.args;
    if (call.name === 'propose_rewrite' && typeof a.replacement === 'string' && a.replacement.trim()) {
      const r = { replacement: a.replacement.trim(), rationale: typeof a.rationale === 'string' ? a.rationale : '' };
      if (typeof a.original_text === 'string' && a.original_text.trim()) r.original_text = a.original_text.trim();
      rewrites.push(r);
    } else if (call.name === 'set_note' && typeof a.note === 'string') {
      note = a.note.trim();
    } else if (call.name === 'define_persona' && name(a) && typeof a.instructions === 'string' && a.instructions.trim()) {
      personas.push({ action: 'define', name: name(a), instructions: a.instructions.trim(), use: a.use === true });
    } else if (call.name === 'use_persona' && name(a)) {
      personas.push({ action: 'use', name: name(a) });
    }
  }
  return { text, rewrites, note, personas, thoughts };
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
