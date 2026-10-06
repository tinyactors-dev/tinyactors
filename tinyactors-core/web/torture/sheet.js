// The components' stylesheet, fetched once and adopted by every shadow root.
// Modules that define elements import it (top-level await), so no element is
// defined, and none renders, before its styles are there.
export const sheet = new CSSStyleSheet();
sheet.replaceSync(await (await fetch('components.css')).text());

/** Attach `el`'s shadow root: the shared sheet, then its <template id> content. */
export function shadow(el, id = el.localName) {
  const root = el.attachShadow({ mode: 'open' });
  root.adoptedStyleSheets = [sheet];
  // importNode (not content.cloneNode): nested custom elements are upgraded
  // at once, even while this element is still detached
  root.append(document.importNode(document.getElementById(id).content, true));
  return root;
}
