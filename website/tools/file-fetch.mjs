// Loaded with `node --import` before an example written for the browser
// (examples/*/browser.mjs): its fetch of a file next to it, by a file: URL,
// answers that file, as a page's fetch of the URL beside it would. Every
// other request goes to Node.js's own fetch.
import { readFile } from 'node:fs/promises';

const fetchOverNetwork = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const url = new URL(input instanceof Request ? input.url : String(input));
  if (url.protocol !== 'file:') return fetchOverNetwork(input, init);
  return new Response(await readFile(url));
};
