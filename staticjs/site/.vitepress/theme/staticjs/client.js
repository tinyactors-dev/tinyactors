// One staticjs for the page, loaded the first time a component needs it.
import { withBase } from 'vitepress';
import { StaticJS } from './runtime.js';

let loading = null;

/** @returns {Promise<StaticJS>} */
export function useStaticJS() {
  loading ??= StaticJS.load(withBase('/staticjs-site.wasm'));
  return loading;
}

/** The UTF-16 index of each UTF-8 byte offset of `text`, and one past the end. */
export function byteToIndex(text) {
  const map = [];
  let index = 0;
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    const bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    for (let b = 0; b < bytes; b++) map.push(index);
    index += ch.length;
  }
  map.push(index);
  return map;
}
