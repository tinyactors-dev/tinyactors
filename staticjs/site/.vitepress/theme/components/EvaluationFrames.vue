<script setup lang="ts">
// The frames of one evaluation as a waterfall, on an axis of fuel: the
// program, each looping built-in and each callback, from when it opens to
// when it closes. Computed in the page: the program runs one unit of fuel at
// a time and its frames are read after each (sjs_vm_frame).
import { onMounted, ref } from 'vue';
import { useStaticJS } from '../staticjs/client.js';
import SpanTree, { type Span } from './ds/SpanTree.vue';

const props = defineProps<{ source: string }>();
const spans = ref<Span[]>([]);
const fuel = ref(0);
const failed = ref('');

const serviceColors = { program: 'var(--data-5)', loop: 'var(--data-1)', callback: 'var(--data-6)' };

/**
 * The arrow a callback frame runs, by its parameters: the function whose
 * body begins last at or before the frame's position, and the parameter
 * list before that body's `=>`.
 */
function callbackName(functions: { at: { offset: number } }[], offset: number) {
  const bytes = new TextEncoder().encode(props.source);
  const fn = [...functions].filter((f) => f.at.offset > 0 && f.at.offset <= offset).sort((a, b) => b.at.offset - a.at.offset)[0];
  if (!fn) return 'function';
  const before = new TextDecoder().decode(bytes.slice(0, fn.at.offset));
  const arrow = before.lastIndexOf('=>');
  if (arrow < 0) return 'function';
  const head = before.slice(0, arrow).trimEnd();
  let start = head.length - 1;
  if (head.endsWith(')')) {
    for (let depth = 0; start >= 0; start--) {
      if (head[start] === ')') depth++;
      if (head[start] === '(' && --depth === 0) break;
    }
  } else {
    while (start > 0 && /[\w$]/.test(head[start - 1])) start--;
  }
  return `${head.slice(Math.max(start, 0))} => …`;
}

onMounted(async () => {
  try {
    const staticjs = await useStaticJS();
    const r = staticjs.pipeline(props.source, { maxSteps: 2000 });
    if (!r.compile.ok || r.status !== 'done') throw new Error('the program did not run');
    fuel.value = r.fuel;
    const out: Span[] = [];
    // The open span at each depth, and what it is.
    const open: { span: Span; key: string; index: number }[] = [];
    let id = 0;
    const close = (depth: number, at: number) => {
      while (open.length > depth) {
        const o = open.pop()!;
        o.span.duration = Math.max(at - o.span.start, 0.5);
      }
    };
    for (const step of r.steps) {
      const frames = step.frames;
      let depth = 0;
      for (; depth < frames.length && depth < open.length; depth++) {
        const f = frames[depth];
        const key = f.kind === 'loop' ? `loop:${f.object}.${f.name}` : depth === 0 ? 'program' : 'callback';
        if (open[depth].key !== key) break;
        if (f.kind === 'loop' && f.length && f.index !== open[depth].index) {
          open[depth].index = f.index;
          (open[depth].span.events ??= []).push({ name: `element ${f.index}`, time: step.fuel });
        }
      }
      close(depth, step.fuel);
      for (; depth < frames.length; depth++) {
        const f = frames[depth];
        const key = f.kind === 'loop' ? `loop:${f.object}.${f.name}` : depth === 0 ? 'program' : 'callback';
        const service = f.kind === 'loop' ? 'loop' : depth === 0 ? 'program' : 'callback';
        const name =
          f.kind === 'loop' ? `${f.object ? `${f.object}.` : ''}${f.name}${f.length ? ` · ${f.length} element${f.length === 1 ? '' : 's'}` : ''}`
          : depth === 0 ? 'the program' : callbackName(r.functions, f.at.offset);
        const span: Span = { spanId: `s${id++}`, parentId: open[depth - 1]?.span.spanId, name, service, start: step.fuel, duration: 0 };
        out.push(span);
        open.push({ span, key, index: f.index });
      }
    }
    close(0, r.fuel);
    spans.value = out;
  } catch (e) {
    failed.value = String((e as Error)?.message ?? e);
  }
});
</script>

<template>
  <div class="evaluation-frames">
    <pre class="program"><code>{{ source }}</code></pre>
    <SpanTree
      v-if="spans.length"
      :spans="spans"
      :service-colors="serviceColors"
      :tree-width="300"
      :row-height="24"
      :format-time="(t: number) => `${Math.round(t)}`"
    />
    <p v-else-if="failed" class="status" data-kind="error">{{ failed }}</p>
    <p v-else class="status" data-kind="busy">Running the program…</p>
    <p v-if="spans.length" class="axis">Fuel, 0 to {{ fuel }}. Ticks on a loop’s bar mark its next element.</p>
  </div>
</template>

<style scoped>
.program {
  margin: 0 0 var(--space-3);
  padding: var(--space-3) var(--space-4);
  font: var(--type-code);
  font-size: 13px;
  color: var(--code-fg);
  background: var(--surface-code);
  border: 1px solid var(--code-border);
  border-radius: var(--radius-2);
  overflow-x: auto;
}
.evaluation-frames :deep(.ta-spantree) {
  overflow-x: auto;
}
.axis {
  margin: var(--space-2) 0 0;
  font: var(--type-body-sm);
  color: var(--fg-3);
}
</style>
