// <ta-live src="/examples/property/live.mjs" call="correct">: an example
// that runs in the reader's browser. Its button (from the page's content)
// imports the module and calls the export `call`, which answers
// { lines, spans? }. The lines show as the run's output; spans, when there
// are any, in a <ta-trace-view>.
// With this module's ?v= query, as every component is loaded.
await import(new URL(`./trace-view.mjs${new URL(import.meta.url).search}`, import.meta.url));

// A live example loads tinyactors (about 500 KB of WebAssembly) before its
// first run. It starts as soon as the example scrolls near the screen, so a
// click usually finds it ready; a click before then says it is loading.
const loading = new Map();
function load(src) {
  if (!loading.has(src)) loading.set(src, import(new URL(src, document.baseURI).href));
  return loading.get(src);
}
const near = new IntersectionObserver((entries) => {
  for (const entry of entries) {
    if (!entry.isIntersecting) continue;
    near.unobserve(entry.target);
    load(entry.target.getAttribute('src')).catch(() => {}); // a failure shows when the button runs it
  }
}, { rootMargin: '600px' });

class Live extends HTMLElement {
  connectedCallback() {
    const button = this.querySelector('button');
    if (!button || button.dataset.ready) return;
    button.dataset.ready = 'true';
    button.disabled = false;
    button.addEventListener('click', () => this.run(button));
    near.observe(this);
  }

  async run(button) {
    const output = this.querySelector('.live__output');
    const code = output.querySelector('code');
    const status = this.querySelector('.live__status');
    let view = this.querySelector('ta-trace-view');
    button.disabled = true;
    const pending = load(this.getAttribute('src'));
    let ready = false;
    pending.then(() => { ready = true; }, () => {});
    await Promise.resolve();
    status.textContent = ready ? 'Running…' : 'Loading tinyactors…';
    try {
      const module = await pending;
      status.textContent = 'Running…';
      const result = await module[this.getAttribute('call')]();
      code.textContent = result.lines.join('\n');
      output.hidden = false;
      if (result.spans?.length) {
        if (!view) {
          view = document.createElement('ta-trace-view');
          this.append(view);
        }
        view.spans = result.spans;
      }
      status.textContent = '';
    } catch (error) {
      code.textContent = String(error?.stack ?? error);
      output.hidden = false;
      status.textContent = 'failed';
    } finally {
      button.disabled = false;
    }
  }
}

if (!customElements.get('ta-live')) customElements.define('ta-live', Live);
