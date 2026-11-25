// <ta-copy>: a block of code a reader can copy (render/site.xsl, f:copyable).
// The page renders the block and a hidden button with Lucide's copy and
// check icons over its top right corner; this shows the button and makes it
// work. The stylesheet shows it while the block is hovered or holds the
// focus, and while `data-active` is set, which a tap in the block sets (a
// touch screen has no hover) and a tap elsewhere clears. A click copies the
// block's text but what is aria-hidden (a command's prompt), and shows the
// check for a moment (`data-copied`), announced by the status's text.

const SHOWN = 1500;

class Copy extends HTMLElement {
  connectedCallback() {
    const button = this.querySelector(':scope > .copy__button');
    if (!button || this.dataset.ready) return;
    this.dataset.ready = 'true';
    button.hidden = false;
    button.addEventListener('click', () => this.copy(button));
    this.addEventListener('pointerdown', (event) => {
      if (event.pointerType !== 'mouse') this.dataset.active = 'true';
    });
  }

  /** The block's text, without what a screen reader skips. */
  text() {
    const block = this.querySelector(':scope > pre, :scope > code').cloneNode(true);
    for (const hidden of block.querySelectorAll('[aria-hidden="true"]')) hidden.remove();
    return block.textContent;
  }

  async copy(button) {
    if (!(await write(this.text()))) return;
    this.dataset.copied = 'true';
    const status = this.querySelector(':scope > .copy__status');
    if (status) status.textContent = button.dataset.done ?? '';
    clearTimeout(this.timer);
    this.timer = setTimeout(() => {
      delete this.dataset.copied;
      if (status) status.textContent = '';
    }, SHOWN);
  }
}

/** Writes `text` to the clipboard; false when the browser refuses. */
async function write(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // no Clipboard API (a page served over plain HTTP) or no permission:
    // the older command, from a selection of a text area of our own
    const area = Object.assign(document.createElement('textarea'), { value: text, readOnly: true });
    area.style.cssText = 'position:fixed;top:0;left:0;opacity:0';
    document.body.append(area);
    area.select();
    try {
      return document.execCommand('copy');
    } catch {
      return false;
    } finally {
      area.remove();
    }
  }
}

// A tap outside a block hides its button again.
document.addEventListener('pointerdown', (event) => {
  for (const block of document.querySelectorAll('ta-copy[data-active]')) {
    if (!block.contains(event.target)) delete block.dataset.active;
  }
});

if (!customElements.get('ta-copy')) customElements.define('ta-copy', Copy);
