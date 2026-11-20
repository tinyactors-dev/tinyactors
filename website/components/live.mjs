// <ta-live src="/examples/property/live.mjs" call="correct">: an example
// that runs in the reader's browser. Its button (from the page's content)
// imports the module and calls the export `call`, which answers
// { lines, spans? }. The lines show as the run's output; spans, when there
// are any, in a <ta-trace-view>.
// With this module's ?v= query, as every component is loaded.
await import(new URL(`./trace-view.mjs${new URL(import.meta.url).search}`, import.meta.url));

class Live extends HTMLElement {
  connectedCallback() {
    const button = this.querySelector('button');
    if (!button || button.dataset.ready) return;
    button.dataset.ready = 'true';
    button.disabled = false;
    button.addEventListener('click', () => this.run(button));
  }

  async run(button) {
    const output = this.querySelector('.live__output');
    const code = output.querySelector('code');
    const status = this.querySelector('.live__status');
    let view = this.querySelector('ta-trace-view');
    button.disabled = true;
    status.textContent = 'running…';
    try {
      const module = await import(new URL(this.getAttribute('src'), document.baseURI).href);
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
