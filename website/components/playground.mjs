// <ta-playground src="/examples/anthill/page.mjs">: a program with controls
// of its own. Its module exports mount(element), which builds it inside the
// element; the description the page wrote stays for screen readers.
class Playground extends HTMLElement {
  async connectedCallback() {
    if (this.dataset.mounted) return;
    this.dataset.mounted = 'true';
    const module = await import(new URL(this.getAttribute('src'), document.baseURI).href);
    this.querySelector('.playground__description')?.classList.add('playground__description--quiet');
    module.mount(this);
  }
}

if (!customElements.get('ta-playground')) customElements.define('ta-playground', Playground);
