import * as ta from '../../bindings/web/index.mjs';
import { editorChart, keyBindings, keyEvent, swapKeymap } from './editor-chart.mjs';

export class TaEditor extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).append(document.importNode(document.getElementById('ta-editor').content, true));
    this.$ = (selector) => this.shadowRoot.querySelector(selector);
    this.options = { keymap: 'vim', escape: 'j j', timeout: 650 };
    const area = this.$('textarea');
    area.addEventListener('keydown', (event) => {
      if (event.isComposing || this.composing || event.key === 'Process' || ['Shift', 'Control', 'Alt', 'Meta', 'Dead', 'Tab'].includes(event.key)) return;
      // Keep browser/OS shortcuts with Meta. Tab leaves the editor normally.
      if (event.metaKey || (event.ctrlKey && ['c', 'v'].includes(event.key.toLowerCase()))) return;
      event.preventDefault();
      const key = event.ctrlKey ? `C-${event.key === ' ' ? 'Space' : event.key.toLowerCase()}` : event.altKey ? `M-${event.key}` : event.key;
      this.actor?.send(keyEvent(key));
    });
    area.addEventListener('beforeinput', (event) => {
      if (this.composing || event.isComposing) return;
      event.preventDefault();
      // Virtual keyboards can emit beforeinput without keydown. Composition's
      // final input is already committed by compositionend, so ignore it here.
      if (event.inputType === 'insertText' && event.data) this.send('TEXT', { text: event.data });
      if (event.inputType === 'deleteContentBackward') this.actor?.send(keyEvent('Backspace'));
      if (event.inputType === 'insertLineBreak') this.actor?.send(keyEvent('Enter'));
    });
    area.addEventListener('paste', (event) => {
      event.preventDefault();
      this.send('TEXT', { text: event.clipboardData.getData('text/plain') });
    });
    area.addEventListener('input', () => {
      if (!this.composing) this.render(this.actor.getSnapshot());
    });
    area.addEventListener('compositionstart', () => {
      this.composing = true;
      this.send('COMPOSITION_START');
    });
    area.addEventListener('compositionend', (event) => {
      this.composing = false;
      this.send('COMPOSITION_END', { text: event.data });
      this.render(this.actor.getSnapshot());
    });
    area.addEventListener('focus', () => this.send('FOCUS'));
    area.addEventListener('blur', () => this.send('BLUR'));
    area.addEventListener('pointerup', () => {
      const end = area.selectionDirection === 'backward' ? area.selectionStart : area.selectionEnd;
      const start = area.selectionDirection === 'backward' ? area.selectionEnd : area.selectionStart;
      this.send('SELECT', { cursor: end, mark: start === end ? null : start });
    });
    area.addEventListener('scroll', () => { this.$('.line-numbers').scrollTop = area.scrollTop; });
    this.$('.save-button').addEventListener('click', () => this.send('SAVE'));
    this.$('.fail-button').addEventListener('click', () => this.send('FAIL_NEXT'));
    this.$('.keymap').addEventListener('change', () => this.upgrade({ keymap: this.$('.keymap').value }));
    this.$('.binding-form').addEventListener('submit', (event) => {
      event.preventDefault();
      const keys = this.$('.binding').value.trim().split(/\s+/);
      if (keys.length < 2 || keys.length > 32 || keys.some((key) => !/^[a-z]$/.test(key))) {
        this.$('.migration').textContent = 'Use 2–32 lowercase keys, separated by spaces.';
        return;
      }
      this.upgrade({ escape: keys.join(' ') });
    });
    this.$('.inspector').addEventListener('click', (event) => {
      if (event.target.closest('button')) this.dispatchEvent(new CustomEvent('inspect-chart', { bubbles: true, composed: true, detail: this.chart }));
    });
  }

  connectedCallback() {
    if (this.actor) return;
    this.system = ta.createSystem();
    this.chart = editorChart(ta, this.options);
    this.actor = ta.createActor(this.chart, { system: this.system }).start();
    this.toggleAttribute('debug', document.body.classList.contains('show-states'));
    this.subscription = this.actor.subscribe((snapshot) => this.render(snapshot));
    this.render(this.actor.getSnapshot());
  }

  disconnectedCallback() {
    this.subscription?.unsubscribe();
    this.system?.stop();
    this.actor = null;
  }

  send(type, extra = {}) { this.actor?.send({ type, ...extra }); }

  async upgrade(changes) {
    if (this.upgrading) return;
    this.upgrading = true;
    this.$('.keymap').disabled = true;
    this.$('.apply').disabled = true;
    this.$('.migration').textContent = 'Compiling SCXML and migrating…';
    try {
      const options = { ...this.options, ...changes };
      await swapKeymap(ta, this.system, this.actor, options);
      this.options = options;
      this.chart = editorChart(ta, options); /* the version it runs now, for the inspector */
      this.$('.migration').textContent = `Chart v${this.actor.getSnapshot().context.version} · same actor, buffer & undo history`;
      this.render(this.actor.getSnapshot());
    } catch (error) {
      this.$('.migration').textContent = error.message;
    } finally {
      this.upgrading = false;
      this.$('.keymap').disabled = false;
      this.$('.apply').disabled = false;
    }
  }

  render(snapshot) {
    const c = snapshot.context, state = snapshot.value.editor; /* the regions of the parallel state */
    const area = this.$('textarea');
    if (!this.composing) {
      const top = area.scrollTop, left = area.scrollLeft;
      if (area.value !== c.text) area.value = c.text;
      let lo = c.cursor, hi = c.cursor;
      if (c.mark != null) { lo = Math.min(c.mark, c.cursor); hi = Math.max(c.mark, c.cursor) + (c.mode === 'visual' ? 1 : 0); }
      else if (['normal', 'operator'].includes(c.mode)) hi = Math.min(c.text.length, lo + 1);
      area.setSelectionRange(lo, hi, c.mark != null && c.cursor < c.mark ? 'backward' : 'forward');
      area.scrollTop = top; area.scrollLeft = left;
    }
    this.$('.line-numbers').textContent = c.text.split('\n').map((_, i) => i + 1).join('\n');
    this.$('.mode-name').textContent = state.mode === 'composing' ? 'COMPOSING' : c.mode === 'operator' ? 'OPERATOR PENDING' : c.mode.toUpperCase();
    this.$('.position').textContent = `Ln ${c.text.slice(0, c.cursor).split('\n').length}, Col ${c.cursor - c.text.lastIndexOf('\n', c.cursor - 1)} · undo ${c.undo.length}`;
    this.$('.save-status').textContent = ({ clean: 'All changes saved', dirty: 'Autosave in 1.8s', saving: 'Saving captured revision…', failed: 'Save failed · retry with Save' })[state.save];
    this.$('.save-status').dataset.state = state.save;
    this.$('.save-button').textContent = state.save === 'failed' ? 'Retry save' : 'Save';
    this.$('.save-button').disabled = state.save === 'saving';
    this.$('.fail-button').setAttribute('aria-pressed', String(c.failNext));
    this.$('.saved-revision').textContent = `buffer r${c.revision} / server r${c.savedRevision}`;
    this.$('.record-status').textContent = c.replaying ? `▶ replay · ${c.playback.length} messages` : c.recording ? `● recording @${c.recording}` : `registers: ${Object.keys(c.macros).join(', ') || 'empty'}`;
    this.$('.notice').textContent = c.notice || 'Tab leaves the editor. Escape cancels a sequence.';
    this.$('.keymap').value = c.keymap;
    this.$('.binding').disabled = c.keymap !== 'vim';
    this.$('.binding').value = this.shadowRoot.activeElement === this.$('.binding') ? this.$('.binding').value : this.options.escape;
    this.$('.vim-recipes').hidden = c.keymap !== 'vim';
    this.$('.emacs-recipes').hidden = c.keymap !== 'emacs';
    for (const mode of ['normal', 'insert', 'visual', 'operator', 'emacs']) {
      const light = this.$(`.light-${mode}`);
      light.dataset.active = String(c.mode === mode);
      light.setAttribute('aria-pressed', String(c.mode === mode));
      light.hidden = c.keymap === 'vim' ? mode === 'emacs' : mode !== 'emacs';
    }
    this.$('.mark-state').textContent = state.mode === 'composing' ? 'IME → remembered mode' : c.mark == null ? 'mark unset' : `mark at ${c.mark}`;
    this.$('.sequence-state').textContent = [c.count && `count ${c.count}`, c.prefix.split(' ').join(' → '), c.motionCount && `× ${c.motionCount}`].filter(Boolean).join(' → ') || 'idle · waiting for a key';
    const pending = !!(c.prefix || c.count);
    this.$('.sequence-panel').dataset.pending = String(pending);
    const token = `${c.prefix}:${c.count}:${c.motionCount}`;
    if (this.lastPrefix !== token) {
      const meter = this.$('.timeout');
      meter.style.animation = 'none';
      void meter.offsetWidth;
      meter.style.animation = pending ? `deadline ${this.options.timeout}ms linear forwards` : 'none';
      this.lastPrefix = token;
    }
    const mapMode = c.mode === 'operator' ? 'normal' : c.mode;
    const bindings = keyBindings(c.keymap, this.options.escape)[mapMode] || {};
    const possible = Object.keys(bindings).filter((key) => c.prefix && key.startsWith(`${c.prefix} `)).slice(0, 5);
    this.$('.possible').textContent = possible.length ? `next: ${possible.map((key) => key.slice(c.prefix.length + 1)).join(' · ')}` : pending ? '650ms to finish this prefix' : 'A key trie compiled into nested states';
    this.$('.save-state').textContent = state.save;
    this.$('.focus-state').textContent = state.focus;
    this.$('.macro-state').textContent = c.recording ? `recording @${c.recording}` : state.macro;
    this.$('.actor-id').textContent = this.actor.address;
  }
}
