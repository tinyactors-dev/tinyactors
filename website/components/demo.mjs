// <ta-demo>: a demo's video and its steps. A step's time seeks the video
// and plays it; the step being shown is marked as the video plays.
class Demo extends HTMLElement {
  connectedCallback() {
    const video = this.querySelector('video');
    const steps = [...this.querySelectorAll('.demo__at')].map((button) => ({ button, at: Number(button.dataset.at) }));
    if (!video || !steps.length) return;
    for (const { button, at } of steps) {
      button.addEventListener('click', () => { video.currentTime = at; video.play().catch(() => {}); });
    }
    video.addEventListener('timeupdate', () => {
      let now = null;
      for (const step of steps) if (step.at <= video.currentTime + 0.25) now = step;
      for (const step of steps) step.button.parentElement.classList.toggle('is-now', step === now);
    });
  }
}
if (!customElements.get('ta-demo')) customElements.define('ta-demo', Demo);
