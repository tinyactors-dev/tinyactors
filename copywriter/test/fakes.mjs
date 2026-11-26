// Fakes of the platform adapters and of Gemini, for the node tests.

import { readFileSync } from 'node:fs';
import { createReviewer } from '../src/core/reviewer.mjs';
import { createMemoryStore } from '../src/core/store.mjs';

/** A page whose text is one string; previews replace in it. */
export function fakePage({ url = 'https://www.tinyactors.dev/', title = 'tinyactors', lang = 'en', text = '', purpose = null } = {}) {
  const previews = new Map(); // id → {exact, text}
  const listeners = [];
  const navigators = [];
  const pickEnds = [];
  const fake = {
    url, title, lang, text, purpose: purpose && (() => fake.declared),
    declared: purpose,
    selected: null,
    highlighted: [],
    calls: [],
    info() { return { url: fake.url, title: fake.title, lang: fake.lang }; },
    context() { return { headings: [fake.title], text: fake.text }; },
    selection() { return fake.selected; },
    onSelection(fn) { listeners.push(fn); },
    onNavigate(fn) { navigators.push(fn); },
    picking: false,
    pick(on) { fake.calls.push(['pick', on]); fake.picking = on; return true; },
    onPickEnd(fn) { pickEnds.push(fn); },
    /** In pick mode, a tap on the element whose text is `exact`: it is the selection. */
    tap(exact) {
      if (!fake.picking) throw new Error('not picking');
      fake.select(exact);
    },
    /** Escape on the page: pick mode ends there. */
    escape() { fake.picking = false; for (const fn of pickEnds) fn(); },
    locate(anchor) { return fake.text.includes(anchor.quote.exact); },
    preview({ id, anchor, text }) {
      fake.calls.push(['preview', id, text]);
      const exact = anchor.quote.exact;
      if (!fake.text.includes(exact)) return false;
      fake.text = fake.text.replace(exact, text);
      previews.set(id, { exact, text });
      return true;
    },
    revert({ id }) {
      fake.calls.push(['revert', id]);
      const p = previews.get(id);
      if (!p || !fake.text.includes(p.text)) return false;
      fake.text = fake.text.replace(p.text, p.exact);
      previews.delete(id);
      return true;
    },
    highlight(anchor) { fake.highlighted.push(anchor); },
    anchor(exact) {
      const at = fake.text.indexOf(exact);
      if (at < 0) return null;
      return {
        quote: { exact, prefix: fake.text.slice(Math.max(0, at - 16), at), suffix: fake.text.slice(at + exact.length, at + exact.length + 16) },
        selector: 'main > p',
      };
    },
    /**
     * Selects `exact` (it must be on the page) and tells the reviewer;
     * `fits: false` as the DOM adapter says of a selection across blocks.
     */
    select(exact, { fits } = {}) {
      const anchor = fake.anchor(exact);
      if (!anchor) throw new Error(`not on the page: ${exact}`);
      fake.selected = { text: exact, anchor, ...(fits === undefined ? {} : { fits }) };
      for (const fn of listeners) fn(fake.selected);
    },
    /** Loads another page (or the same again) and tells the reviewer. */
    navigate(next) {
      Object.assign(fake, next);
      previews.clear();
      for (const fn of navigators) fn();
    },
  };
  return fake;
}

/**
 * The site's audience and voice files, from test/fixtures/audiences/ and
 * test/fixtures/voices/ (fetched once from https://www.tinyactors.dev/audiences/
 * and /voices/): `fetchFile(url)` gives a file's
 * text by its name; `files` overrides or adds texts by URL; `asked` lists
 * the URLs asked for.
 */
export function fakeFiles(files = {}) {
  const asked = [];
  const fetchFile = async (url) => {
    asked.push(url);
    if (url in files) {
      if (files[url] instanceof Error) throw files[url];
      return files[url];
    }
    const path = new URL(url).pathname;
    const name = path.split('/').pop();
    const dir = path.includes('/voices/') ? 'voices' : 'audiences';
    return readFileSync(new URL(`./fixtures/${dir}/${name}`, import.meta.url), 'utf8');
  };
  fetchFile.asked = asked;
  fetchFile.files = files;
  return fetchFile;
}

/** The fixture audience file `name` (agent-platform, tech-lead, workflow-tester) as text. */
export const audienceFile = (name) => readFileSync(new URL(`./fixtures/audiences/${name}.xml`, import.meta.url), 'utf8');

/** Credentials in memory. */
export function fakeCredentials(initial = null) {
  let key = initial;
  let remember = false;
  return {
    get: () => key,
    set: (k, { remember: r } = {}) => { key = k; remember = Boolean(r); },
    forget: () => { key = null; },
    remembered: () => remember,
  };
}

/** A reply of generateContent: text and function calls. */
export function geminiReply(text, calls = []) {
  return {
    candidates: [{
      content: {
        role: 'model',
        parts: [
          ...(text ? [{ text }] : []),
          ...calls.map(([name, args]) => ({ functionCall: { name, args } })),
        ],
      },
    }],
  };
}

/**
 * A fetch that answers from a queue: each entry a body (status 200) or
 * `{status, body}`. `requests` holds what was asked.
 */
export function fakeFetch(...answers) {
  const queue = [...answers];
  const requests = [];
  const fetch = async (url, init = {}) => {
    requests.push({ url: String(url), headers: init.headers || {}, body: init.body ? JSON.parse(init.body) : undefined });
    if (queue.length === 0) throw new Error(`fakeFetch: no answer left for ${url}`);
    let answer = queue.shift();
    if (typeof answer === 'function') answer = await answer(String(url));
    const { status = 200, body } = answer && answer.status ? answer : { body: answer };
    return new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });
  };
  fetch.requests = requests;
  fetch.answer = (...more) => queue.push(...more);
  fetch.queue = queue;
  return fetch;
}

/** A reviewer over fakes; returns everything a test pokes. */
export async function setup({ text = 'Build better products. Actors are cheap. Read the guide.', key = 'AIza-test-key-123', fetch = fakeFetch(), store = createMemoryStore(), page = fakePage({ text }), recorder, fetchFile = fakeFiles(), fetchPage = null, retryDelays = [] } = {}) {
  const credentials = fakeCredentials(key);
  const downloads = [];
  const clipboard = [];
  const reviewer = createReviewer({
    page, credentials, store, fetch, recorder, fetchFile, fetchPage, retryDelays,
    download: (name, bytes) => downloads.push({ name, bytes }),
    clipboard: (t) => clipboard.push(t),
  });
  await reviewer.ready();
  return { reviewer, page, credentials, fetch, store, downloads, clipboard };
}

export const find = (reviewer, id) => reviewer.getState().annotations.find((a) => a.id === id);
