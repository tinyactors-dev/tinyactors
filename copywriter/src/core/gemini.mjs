// Gemini's generateContent over REST, with the copywriter's two functions
// and audio clips as inline data. The key goes in the `x-goog-api-key`
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
]);

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

/** The text and the function calls of a generateContent response. */
export function parse(body) {
  const candidate = (body.candidates || [])[0];
  if (!candidate) {
    const reason = body.promptFeedback?.blockReason;
    throw new GeminiError(reason ? `Gemini blocked the request (${reason}).` : 'Gemini gave no answer.');
  }
  const texts = [];
  const calls = [];
  for (const part of candidate.content?.parts || []) {
    if (part.thought) continue;
    if (typeof part.text === 'string') texts.push(part.text);
    if (part.functionCall) calls.push({ name: part.functionCall.name, args: part.functionCall.args || {} });
  }
  return { text: texts.join('').trim(), calls };
}

/**
 * One generateContent request: `{text, calls}`, or a GeminiError whose
 * message says what to do.
 */
export async function generate({ apiKey, model, system, contents, fetch = globalThis.fetch }) {
  if (!apiKey) throw new GeminiError('No Gemini key: add yours in Settings.');
  const request = {
    systemInstruction: { parts: [{ text: system }] },
    contents,
    tools: [{ functionDeclarations: FUNCTIONS }],
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
    throw new GeminiError(explain(response.status, detail, reason), { status: response.status });
  }
  return parse(await response.json());
}

/**
 * What the copywriter's processors need from a reply: the text, the
 * rewrites (`propose_rewrite`'s arguments) and a note (`set_note`). Calls
 * of other names, or with the wrong types, are dropped.
 */
export function reading({ text, calls }) {
  const rewrites = [];
  let note = null;
  for (const call of calls) {
    const a = call.args;
    if (call.name === 'propose_rewrite' && typeof a.replacement === 'string' && a.replacement.trim()) {
      const r = { replacement: a.replacement.trim(), rationale: typeof a.rationale === 'string' ? a.rationale : '' };
      if (typeof a.original_text === 'string' && a.original_text.trim()) r.original_text = a.original_text.trim();
      rewrites.push(r);
    } else if (call.name === 'set_note' && typeof a.note === 'string') {
      note = a.note.trim();
    }
  }
  return { text, rewrites, note };
}
