// Jev, TypeSafe's decision model, through OpenRouter's decisions API
// (POST https://openrouter.ai/api/alpha/decisions: {model, state,
// questions} -> {answers}). The chart writes the request; this sends it.
// Without a key every request answers decide.unavailable, and the project
// files the report as new, as it does when the model is down.

export class Jev {
  constructor({ key, model = 'typesafe/jev-1.13', fetch: fetcher = globalThis.fetch } = {}) {
    this.key = key; this.model = model; this.fetch = fetcher;
  }
  processor = async (event, data) => {
    if (event !== 'decide') return null;
    if (!this.key) return { event: 'decide.unavailable', data: { reason: 'no key' } };
    try {
      const response = await this.fetch('https://openrouter.ai/api/alpha/decisions', {
        method: 'POST', signal: AbortSignal.timeout(4000),
        headers: { authorization: `Bearer ${this.key}`, 'content-type': 'application/json' },
        body: JSON.stringify({ ...data, model: this.model }),
      });
      if (!response.ok) return { event: 'decide.unavailable', data: { status: response.status } };
      const body = await response.json();
      return { event: 'decided', data: { model: body.model ?? this.model, answers: body.answers } };
    } catch (error) {
      return { event: 'decide.unavailable', data: { reason: String(error.message ?? error) } };
    }
  };
}
