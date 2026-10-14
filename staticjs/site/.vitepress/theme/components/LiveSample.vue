<script setup lang="ts">
// A live sample: the code, highlighted, and its value, computed when the
// site was built. "Edit" turns the code into an editor that runs in this
// page, through staticjs.wasm, as you type.
import { computed, nextTick, ref, watch } from 'vue';
import { useStaticJS } from '../staticjs/client.js';
import { describeError } from '../staticjs/runtime.js';
import Icon from './Icon.vue';

const props = defineProps<{ source: string; initial: string; module?: boolean }>();
const original = decodeURIComponent(props.source);
const code = ref(original);
const result = ref(JSON.parse(decodeURIComponent(props.initial)));
const editing = ref(false);
const running = ref(false);
const editor = ref<HTMLTextAreaElement | null>(null);
let timer: ReturnType<typeof setTimeout> | undefined;

const rows = computed(() => Math.max(2, code.value.split('\n').length));
const changed = computed(() => code.value !== original);

/** The source line an error points at, and a caret under its column. */
const pointer = computed(() => {
  const error = result.value.error;
  if (!error || !error.line || !code.value.includes('\n')) return null;
  const line = code.value.split('\n')[error.line - 1] ?? '';
  const bytes = new TextEncoder().encode(line);
  const column = new TextDecoder().decode(bytes.slice(0, Math.max(0, error.column - 1))).length;
  return { line, caret: `${' '.repeat(column)}^` };
});

async function run() {
  running.value = true;
  try {
    const staticjs = await useStaticJS();
    result.value = staticjs.evaluate(code.value, { module: props.module });
  } catch (e) {
    result.value = { ok: false, error: { kind: 'error', message: String(e), line: 0, column: 0, subject: '' } };
  } finally {
    running.value = false;
  }
}

async function edit() {
  editing.value = true;
  useStaticJS();
  await nextTick();
  editor.value?.focus();
}

function reset() {
  code.value = original;
  result.value = JSON.parse(decodeURIComponent(props.initial));
}

watch(code, () => {
  if (!editing.value) return;
  clearTimeout(timer);
  timer = setTimeout(run, 180);
});

function onKey(event: KeyboardEvent) {
  if ((event.metaKey || event.ctrlKey) && event.key === 'Enter') run();
  if (event.key === 'Tab') {
    event.preventDefault();
    const el = event.target as HTMLTextAreaElement;
    const { selectionStart: a, selectionEnd: b } = el;
    code.value = `${code.value.slice(0, a)}  ${code.value.slice(b)}`;
    nextTick(() => el.setSelectionRange(a + 2, a + 2));
  }
}
</script>

<template>
  <figure class="live-sample" :class="{ editing }">
    <div v-show="!editing" class="code"><slot /></div>
    <div v-if="editing" class="editor">
      <textarea
        ref="editor"
        v-model="code"
        :rows="rows"
        spellcheck="false"
        autocapitalize="off"
        autocomplete="off"
        aria-label="staticjs source"
        @keydown="onKey"
      ></textarea>
    </div>
    <div class="out" :class="result.ok ? 'ok' : 'err'" aria-live="polite">
      <template v-if="result.ok">
        <span class="arrow" aria-hidden="true">→</span>
        <code class="value">{{ result.value }}</code>
        <span class="meta" :title="`evaluated on ${result.fuel} units of fuel`">{{ result.kind }}, {{ result.fuel }} fuel</span>
      </template>
      <template v-else>
        <span class="status" data-kind="error">{{ describeError(result.error) }}</span>
        <pre v-if="pointer" class="pointer">{{ pointer.line }}
<span>{{ pointer.caret }}</span></pre>
      </template>
      <span class="controls">
        <button v-if="changed" class="icon-button" type="button" title="Reset to the original" aria-label="Reset to the original" @click="reset"><Icon name="reset" /></button>
        <button v-if="!editing" class="icon-button" type="button" title="Edit and run it here" aria-label="Edit this sample and run it in the page" @click="edit"><Icon name="pencil" /></button>
        <button v-else class="icon-button primary" type="button" title="Run (⌘↵)" aria-label="Run" :disabled="running" @click="run"><Icon name="play" /></button>
      </span>
    </div>
  </figure>
</template>

<style scoped>
.live-sample {
  margin: 20px 0;
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  background: var(--surface-code);
  overflow: hidden;
}
.controls {
  display: inline-flex;
  gap: 6px;
  margin-left: auto;
  align-self: center;
}
.code :deep(div[class*='language-']) {
  margin: 0 !important;
  border: 0 !important;
  border-radius: 0 !important;
}
.code :deep(span.lang) {
  display: none;
}
.editor textarea {
  display: block;
  width: 100%;
  box-sizing: border-box;
  padding: 20px 24px;
  margin: 0;
  border: 0;
  resize: vertical;
  font-family: var(--font-mono);
  font-size: var(--vp-code-font-size);
  line-height: var(--vp-code-line-height);
  color: var(--code-fg);
  background: var(--surface-code);
  tab-size: 2;
  outline: none;
}
.editor textarea:focus {
  box-shadow: inset 0 0 0 2px var(--focus-ring);
}
.out {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--space-2) var(--space-3);
  padding: 8px 8px 8px 14px;
  border-top: 1px solid var(--code-border);
  background: var(--surface-1);
  font-size: 14px;
}
.out.err {
  box-shadow: inset 3px 0 0 var(--state-error);
}
.arrow {
  color: var(--accent-fg);
  font-family: var(--font-mono);
}
.value {
  flex: 1;
  min-width: 0;
  font-family: var(--font-mono);
  font-size: 13.5px;
  color: var(--fg-1);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  background: none !important;
  border: 0 !important;
  padding: 0 !important;
}
.meta {
  font: var(--type-body-sm);
  font-size: 13px;
  color: var(--fg-3);
}
.out .status {
  color: var(--fg-1);
  font: var(--type-body-sm);
}
.pointer {
  order: 5;
  flex-basis: 100%;
  margin: 4px 0 0;
  padding: 0;
  font: var(--type-code);
  color: var(--fg-2);
  background: none;
  overflow-x: auto;
}
.pointer span {
  color: var(--state-error);
  font-weight: 600;
}
</style>
