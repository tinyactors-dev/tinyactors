// Jev, TypeSafe's decision model, through TypeSafe's own API
// (POST https://api.typesafe.ai/v1/systemone: {model, state, questions} ->
// {model, answers, usage}). The chart writes the request; this sends it.
// `jev-latest` is TypeSafe's floating id (jev-1.14.0 on 2026-11-27); the
// ledger records the model each answer names, so a threshold tuned on one
// version can be checked again on the next.
// Without a key every request answers decide.unavailable, and the project
// files the report as new, as it does when the model is down.

export class Jev {
  constructor({ key, model = 'jev-latest', fetch: fetcher = globalThis.fetch } = {}) {
    this.key = key; this.model = model; this.fetch = fetcher;
  }
  processor = async (event, data) => {
    if (event !== 'decide') return null;
    if (!this.key) return { event: 'decide.unavailable', data: { reason: 'no key' } };
    try {
      const response = await this.fetch('https://api.typesafe.ai/v1/systemone', {
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
