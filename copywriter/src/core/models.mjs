// The models the copywriter can ask. The list comes from Google's
// models.list with the user's key; until it loads, or when it fails, the
// hard-coded list below is used.

export const BASE = 'https://generativelanguage.googleapis.com/v1beta';

/** The default model: Gemini 4 Hydrogen (function calling, audio input). */
export const DEFAULT_MODEL = 'gemini-4-hydrogen';

/** The list used before models.list answers, or when it fails. */
export const MODELS = Object.freeze([
  { id: 'gemini-4-hydrogen', label: 'Gemini 4 Hydrogen' },
  { id: 'gemini-4-carbon', label: 'Gemini 4 Carbon' },
  { id: 'gemini-4-argon', label: 'Gemini 4 Argon' },
  { id: 'gemini-4-neon', label: 'Gemini 4 Neon' },
  { id: 'gemini-3.8-flash', label: 'Gemini 3.8 Flash' },
  { id: 'gemini-3.5-flash-lite', label: 'Gemini 3.5 Flash-Lite' },
].map(Object.freeze));

/** Models that answer generateContent but cannot hold a conversation
 * about copy (speech, images, music, agents), and the moving aliases. */
const NOT_FOR_COPY = /tts|image|transcribe|robotics|computer-use|customtools|nano-banana|embedding|live|omni|-latest$/;

/** Newer generations first: gemini-4-* before gemini-3.8-* before 2.5. */
const generation = (id) => Number((id.match(/^gemini-(\d+(?:\.\d+)?)/) || [])[1] || 0);

/**
 * Lists the `gemini-*` models that support generateContent, with the
 * user's key: `{models: [{id, label}], fallback}`, the default first when
 * it is listed. On any failure (or no key) it is the hard-coded list and
 * `fallback: true`.
 */
export async function listModels(apiKey, { fetch = globalThis.fetch } = {}) {
  if (!apiKey) return { models: MODELS, fallback: true };
  try {
    const models = [];
    let pageToken = '';
    for (let pages = 0; pages < 10; pages++) {
      const url = `${BASE}/models?pageSize=1000${pageToken ? `&pageToken=${encodeURIComponent(pageToken)}` : ''}`;
      const response = await fetch(url, { headers: { 'x-goog-api-key': apiKey } });
      if (!response.ok) throw new Error(`models.list answered ${response.status}`);
      const body = await response.json();
      for (const m of body.models || []) {
        const id = String(m.name || '').replace(/^models\//, '');
        if (!id.startsWith('gemini-')) continue;
        if (!(m.supportedGenerationMethods || []).includes('generateContent')) continue;
        if (NOT_FOR_COPY.test(id)) continue;
        models.push({ id, label: m.displayName || id });
      }
      pageToken = body.nextPageToken;
      if (!pageToken) break;
    }
    if (models.length === 0) return { models: MODELS, fallback: true };
    models.sort((a, b) => (b.id === DEFAULT_MODEL) - (a.id === DEFAULT_MODEL)
      || generation(b.id) - generation(a.id) || a.id.localeCompare(b.id));
    return { models, fallback: false };
  } catch {
    return { models: MODELS, fallback: true };
  }
}
