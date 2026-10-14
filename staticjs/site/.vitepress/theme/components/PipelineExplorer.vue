<script setup lang="ts">
// The pipeline explorer: a program's way through staticjs, stage by stage,
// computed in this page by staticjs.wasm. Every number on it comes from the
// library's public API (the site's C layer only records what it sees).
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { byteToIndex, useStaticJS } from '../staticjs/client.js';
import { describeError, operator } from '../staticjs/runtime.js';

type Position = { offset: number; line: number; column: number };

const examples = [
  {
    id: 'sort',
    name: 'Sort and pluck',
    module: false,
    source: '[\n  { name: "ada", age: 36 },\n  { name: "grace", age: 85 },\n  { name: "linus", age: 54 },\n]\n  .toSorted((a, b) => b.age - a.age)\n  .map((p) => p.name)',
  },
  {
    id: 'module',
    name: 'A module',
    module: true,
    source: 'const square = (n) => n * n;\n\nexport const total = [1, 2, 3, 4].map(square).reduce((sum, n) => sum + n, 0);\nexport default total > 20;',
  },
  {
    id: 'mutation',
    name: 'Owned mutation',
    module: false,
    source: '["b", "a", "c", "a"].reduce((seen, x) => {\n  seen.add(x);\n  return seen;\n}, new Set())',
  },
  {
    id: 'bigint',
    name: 'BigInt',
    module: false,
    source: '[1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n, 9n, 10n, 11n, 12n, 13n, 14n, 15n]\n  .reduce((a, b) => a * b, 1n)',
  },
  {
    id: 'temporal',
    name: 'Temporal',
    module: false,
    source: 'Temporal.PlainDate.from("2024-01-31")\n  .add({ months: 1 })\n  .toString()',
  },
  { id: 'fault', name: 'A fault', module: false, source: '[{ total: 1 }, null, { total: 3 }]\n  .map((order) => order.total)' },
  { id: 'refused', name: 'Refused', module: false, source: '(xs) => {\n  let sum = 0;\n  for (const x of xs) sum += x;\n  return sum;\n}' },
  { id: 'syntax', name: 'Not JavaScript', module: false, source: '[1, 2, 3].map((x) => x * )' },
];

const stages = [
  { id: 'parser', number: '01', name: 'Parser', blurb: 'bytes in, events out' },
  { id: 'compiler', number: '02', name: 'Compiler', blurb: 'events in, a program out' },
  { id: 'evaluator', number: '03', name: 'Evaluator', blurb: 'fuel in, a value out' },
  { id: 'writer', number: '04', name: 'Writer', blurb: 'a value in, text out' },
] as const;

const chunkSizes = [
  { value: 0, label: 'whole' },
  { value: 16, label: '16 bytes' },
  { value: 4, label: '4 bytes' },
  { value: 1, label: '1 byte' },
];

const example = ref(examples[0].id);
const source = ref(examples[0].source);
const isModule = ref(examples[0].module);
const chunk = ref(4);
const stage = ref<(typeof stages)[number]['id']>('parser');
const result = ref<any>(null);
const loading = ref(true);
const failed = ref('');
const step = ref(0);
const playing = ref(false);
const hovered = ref<number | null>(null);
const viewEl = ref<HTMLElement | null>(null);
function syncScroll(event: Event) {
  const t = event.target as HTMLTextAreaElement;
  if (viewEl.value) {
    viewEl.value.scrollTop = t.scrollTop;
    viewEl.value.scrollLeft = t.scrollLeft;
  }
}
let timer: ReturnType<typeof setTimeout> | undefined;
let player: ReturnType<typeof setInterval> | undefined;

function choose(id: string) {
  const e = examples.find((x) => x.id === id)!;
  example.value = id;
  source.value = e.source;
  isModule.value = e.module;
}

async function analyse() {
  try {
    const staticjs = await useStaticJS();
    const r = staticjs.pipeline(source.value, { module: isModule.value, chunk: chunk.value });
    result.value = r;
    failed.value = '';
    stop();
    step.value = r.steps ? r.steps.length - 1 : 0;
    hovered.value = null;
    const where = errorStage.value;
    if (where && stage.value !== where && stageIndex(stage.value) > stageIndex(where)) stage.value = where as any;
  } catch (e) {
    failed.value = String(e);
  } finally {
    loading.value = false;
  }
}

onMounted(analyse);
onBeforeUnmount(stop);
watch([source, isModule, chunk], () => {
  clearTimeout(timer);
  timer = setTimeout(analyse, 200);
});

const stageIndex = (id: string) => stages.findIndex((s) => s.id === id);

/** Where the program stopped, if it did: the stage of its error. */
const errorStage = computed(() => {
  const r = result.value;
  if (!r) return null;
  if (!r.compile.ok) return r.compile.error?.stage === 'parser' ? 'parser' : 'compiler';
  if (r.status === 'fault') return 'evaluator';
  return null;
});
const error = computed(() => {
  const r = result.value;
  if (!r) return null;
  return r.compile.ok ? r.error ?? null : r.compile.error;
});

function stageState(id: string) {
  const at = errorStage.value;
  if (!at) return 'ok';
  const a = stageIndex(id);
  const b = stageIndex(at);
  return a < b ? 'ok' : a === b ? 'error' : 'idle';
}

function stageStat(id: string) {
  const r = result.value;
  if (!r) return '';
  switch (id) {
    case 'parser':
      return `${r.bytes} bytes · ${r.compile.chunks} chunk${r.compile.chunks === 1 ? '' : 's'} · ${r.events.length} events`;
    case 'compiler':
      return r.compile.ok ? `${r.functions.length} function${r.functions.length === 1 ? '' : 's'} · ${r.compile.programBytes} bytes` : 'refused';
    case 'evaluator':
      return r.compile.ok ? `${r.fuel} fuel${r.status === 'fault' ? ' · faulted' : ''}` : 'not reached';
    case 'writer':
      return r.output ? `${r.output.text.length} chars · ${r.output.pieces} pieces` : 'not reached';
  }
  return '';
}

// ── the source, with what the selected stage points at ────────────────
const indexOf = computed(() => byteToIndex(source.value));
const toIndex = (offset: number) => indexOf.value[Math.min(offset, indexOf.value.length - 1)] ?? 0;

/** The extent of the token at `index`: a word, a string, or one character. */
function tokenEnd(index: number) {
  const s = source.value;
  const c = s[index];
  if (c === undefined) return index + 1;
  if (/[A-Za-z0-9_$]/.test(c)) {
    let j = index;
    while (j < s.length && /[A-Za-z0-9_$.]/.test(s[j]) && !(s[j] === '.' && !/[0-9]/.test(s[j + 1] ?? ''))) j++;
    return Math.max(j, index + 1);
  }
  if (c === '"' || c === "'" || c === '`') {
    let j = index + 1;
    while (j < s.length && s[j] !== c) j += s[j] === '\\' ? 2 : 1;
    return Math.min(j + 1, s.length);
  }
  if (/[=!<>&|?*]/.test(c)) {
    let j = index;
    while (j < s.length && /[=!<>&|?*]/.test(s[j]) && j - index < 3) j++;
    return j;
  }
  return index + 1;
}

const frames = computed(() => result.value?.steps?.[step.value]?.frames ?? []);

/** Character classes for the source view: chunk shading, spans, markers. */
const marks = computed(() => {
  const r = result.value;
  const s = source.value;
  const chars = Array.from({ length: s.length }, () => ({ chunk: 0, span: false, token: false, frame: 0, error: false }));
  if (!r) return chars;
  if (chunk.value > 0) {
    for (let b = 0; b < r.bytes; b++) {
      const i = toIndex(b);
      if (chars[i]) chars[i].chunk = Math.floor(b / chunk.value) % 2;
    }
  }
  if (stage.value === 'parser' && hovered.value !== null) {
    const ev = r.events[hovered.value];
    if (ev) {
      const a = toIndex(ev.start.offset);
      const at = toIndex(ev.at.offset);
      const end = tokenEnd(at);
      for (let i = a; i < end && i < s.length; i++) chars[i].span = true;
      for (let i = at; i < end && i < s.length; i++) chars[i].token = true;
    }
  }
  if (stage.value === 'evaluator') {
    frames.value.forEach((f: any, depth: number) => {
      const at = toIndex(f.at.offset);
      const end = tokenEnd(at);
      const innermost = depth === frames.value.length - 1;
      for (let i = at; i < end && i < s.length; i++) chars[i].frame = innermost ? 2 : Math.max(chars[i].frame, 1);
    });
  }
  if (stage.value === 'compiler' && r.functions && hovered.value !== null) {
    const fn = r.functions[hovered.value];
    if (fn) {
      const at = toIndex(fn.at.offset);
      for (let i = at; i < tokenEnd(at); i++) chars[i].token = true;
    }
  }
  const e = error.value;
  if (e && e.line) {
    const at = toIndex(e.offset);
    for (let i = at; i < tokenEnd(at) && i < s.length; i++) chars[i].error = true;
  }
  return chars;
});

/** The source as lines of runs of characters with the same marks. */
const lines = computed(() => {
  const s = source.value;
  const m = marks.value;
  const out: { runs: { text: string; cls: string }[] }[] = [{ runs: [] }];
  let current = '';
  let cls = '';
  const flush = () => {
    if (current) out[out.length - 1].runs.push({ text: current, cls });
    current = '';
  };
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '\n') {
      flush();
      out.push({ runs: [] });
      continue;
    }
    const k = m[i];
    const c = [k.chunk ? 'c1' : 'c0', k.span ? 'span' : '', k.token ? 'tok' : '', k.frame === 2 ? 'now' : k.frame === 1 ? 'outer' : '', k.error ? 'err' : '']
      .filter(Boolean).join(' ');
    if (c !== cls) {
      flush();
      cls = c;
    }
    current += s[i];
  }
  flush();
  return out;
});

// ── the parser's events, indented by the brackets they open ───────────
const events = computed(() => {
  const r = result.value;
  if (!r) return [];
  let depth = 0;
  return r.events.map((ev: any, i: number) => {
    if (/_END$/.test(ev.kind) || ev.kind === 'PARAMS_END') depth = Math.max(0, depth - 1);
    const row = { i, depth, ev, payload: payload(ev) };
    if (/_BEGIN$/.test(ev.kind)) depth++;
    return row;
  });
});

function payload(ev: any) {
  if (ev.kind === 'NUMBER') return String(ev.number);
  if (['UNARY', 'BINARY', 'LOGICAL', 'LOGICAL_END'].includes(ev.kind)) return operator(ev.count);
  if (ev.text) return ['STRING'].includes(ev.kind) ? JSON.stringify(ev.text) : ev.text;
  if (['CALL_END', 'NEW_END', 'ARROW_BEGIN', 'TEMPLATE_END'].includes(ev.kind)) return `${ev.count}`;
  return '';
}

// ── the evaluator's steps ─────────────────────────────────────────────
const steps = computed(() => result.value?.steps ?? []);
const depthPath = computed(() => {
  const s = steps.value;
  if (s.length < 2) return '';
  const max = Math.max(1, ...s.map((x: any) => x.frames.length));
  const w = 600;
  const h = 48;
  return s.map((x: any, i: number) => `${i === 0 ? 'M' : 'L'}${((i / (s.length - 1)) * w).toFixed(1)},${(h - (x.frames.length / max) * (h - 4)).toFixed(1)}`).join(' ');
});
const cursorX = computed(() => (steps.value.length > 1 ? (step.value / (steps.value.length - 1)) * 600 : 0));

function play() {
  if (playing.value) return stop();
  if (step.value >= steps.value.length - 1) step.value = 0;
  playing.value = true;
  player = setInterval(() => {
    if (step.value >= steps.value.length - 1) return stop();
    step.value++;
  }, Math.max(16, 2400 / Math.max(1, steps.value.length)));
}
function stop() {
  playing.value = false;
  clearInterval(player);
}
function move(by: number) {
  stop();
  step.value = Math.min(Math.max(0, step.value + by), Math.max(0, steps.value.length - 1));
}

function frameLabel(f: any, i: number) {
  if (f.kind === 'loop') return `${f.object ? `${f.object}.` : ''}${f.name}`;
  return i === 0 ? (isModule.value ? 'module' : 'program') : 'function';
}

// ── the writer's pieces ───────────────────────────────────────────────
const pieces = computed(() => {
  const r = result.value;
  if (!r?.output) return [];
  const bytes = new TextEncoder().encode(r.output.text);
  const out = [];
  for (let i = 0; i < bytes.length; i += r.pieceSize) out.push(new TextDecoder().decode(bytes.slice(i, i + r.pieceSize)));
  return out;
});

const fuelSaved = computed(() => {
  const r = result.value;
  if (!r?.fuel || !r.fuelWithoutShapes || r.status !== 'done') return null;
  return { with: r.fuel, without: r.fuelWithoutShapes, ratio: r.fuel / r.fuelWithoutShapes };
});
</script>

<template>
  <section class="pipeline">
    <div class="examples" role="tablist" aria-label="Examples">
      <button
        v-for="e in examples"
        :key="e.id"
        role="tab"
        type="button"
        :aria-selected="example === e.id"
        :class="{ selected: example === e.id }"
        @click="choose(e.id)"
      >{{ e.name }}</button>
    </div>

    <div class="workbench">
      <div class="input panel">
        <header class="panel-head">
          <span class="eyebrow">Source</span>
          <label class="toggle"><input v-model="isModule" type="checkbox" /> module</label>
          <label class="select">push
            <select v-model.number="chunk" aria-label="Chunk size">
              <option v-for="c in chunkSizes" :key="c.value" :value="c.value">{{ c.label }}</option>
            </select>
          </label>
        </header>
        <div class="code-area">
          <div ref="viewEl" class="view" aria-hidden="true">
            <div v-for="(line, n) in lines" :key="n" class="line">
              <span class="ln">{{ n + 1 }}</span><span class="text"><span v-for="(run, k) in line.runs" :key="k" :class="run.cls">{{ run.text }}</span>{{ '\u200b' }}</span>
            </div>
          </div>
          <textarea
            v-model="source"
            class="editor"
            spellcheck="false"
            autocapitalize="off"
            autocomplete="off"
            wrap="off"
            aria-label="Program source"
            @input="example = ''"
            @scroll="syncScroll"
          ></textarea>
        </div>
        <p class="legend">
          <span v-if="chunk"><i class="sw c1"></i>alternate {{ chunk }}-byte chunks, pushed in turn</span>
          <span v-if="stage === 'parser'"><i class="sw span"></i>the hovered event's node</span>
          <span v-if="stage === 'evaluator'"><i class="sw now"></i>running now <i class="sw outer"></i>waiting</span>
          <span v-if="error"><i class="sw err"></i>{{ error.kind }}</span>
        </p>
      </div>

      <div class="flow">
        <ol class="stages">
          <li v-for="s in stages" :key="s.id">
            <button
              type="button"
              class="stage"
              :class="[stageState(s.id), { selected: stage === s.id }]"
              :aria-pressed="stage === s.id"
              @click="stage = s.id; hovered = null"
            >
              <span class="num">{{ s.number }}</span>
              <span class="name">{{ s.name }}</span>
              <span class="blurb">{{ s.blurb }}</span>
              <span class="stat">{{ loading ? '…' : stageStat(s.id) }}</span>
            </button>
          </li>
        </ol>

        <div class="detail panel" aria-live="polite">
          <p v-if="failed" class="problem">{{ failed }}</p>
          <p v-else-if="loading" class="muted">Loading staticjs.wasm…</p>
          <template v-else-if="result">
            <div v-if="error && errorStage === stage" class="error-box">
              <span class="chip error">{{ error.kind }}</span>
              <span>{{ describeError(error) }}</span>
            </div>

            <!-- 01 the parser -->
            <div v-if="stage === 'parser'">
              <p class="explain">The parser is pushed the source {{ chunk ? `${chunk} byte${chunk === 1 ? '' : 's'} at a time` : 'whole' }} and sends each piece of syntax to its sink as an event, in evaluation order: operands before their operator. Hover an event to see its node in the source.</p>
              <ol class="events" @mouseleave="hovered = null">
                <li
                  v-for="row in events"
                  :key="row.i"
                  :class="{ on: hovered === row.i }"
                  @mouseenter="hovered = row.i"
                  @focus="hovered = row.i"
                  tabindex="0"
                >
                  <span class="idx">{{ row.i }}</span>
                  <span class="kind" :style="{ paddingLeft: `${row.depth * 14}px` }">{{ row.ev.kind }}</span>
                  <code class="pl">{{ row.payload }}</code>
                  <span class="pos">node {{ row.ev.node }} · {{ row.ev.at.line }}:{{ row.ev.at.column }}<template v-if="chunk"> · chunk {{ row.ev.chunk + 1 }}</template></span>
                </li>
              </ol>
            </div>

            <!-- 02 the compiler -->
            <div v-else-if="stage === 'compiler'">
              <p class="explain">The compiler is the parser's sink: it turns events into bytecode as they arrive, and tags each function whose shape a built-in can run in C.</p>
              <template v-if="result.compile.ok">
                <dl class="facts">
                  <div><dt>Program</dt><dd>{{ result.compile.programBytes }} bytes</dd></div>
                  <div><dt>Functions</dt><dd>{{ result.functions.length }}</dd></div>
                  <div><dt>Shaped</dt><dd>{{ result.functions.filter((f: any) => f.shape).length }}</dd></div>
                </dl>
                <table class="fns">
                  <thead><tr><th>#</th><th>begins</th><th>params</th><th>shape</th></tr></thead>
                  <tbody @mouseleave="hovered = null">
                    <tr v-for="(f, i) in result.functions" :key="i" :class="{ on: hovered === i }" @mouseenter="hovered = i">
                      <td>{{ i === 0 ? 'main' : i }}</td>
                      <td>{{ f.at.line }}:{{ f.at.column }}</td>
                      <td>{{ f.params }}</td>
                      <td><span v-if="f.shape" class="chip running">{{ f.shape }}</span><span v-else class="muted">—</span></td>
                    </tr>
                  </tbody>
                </table>
                <div v-if="fuelSaved" class="bars">
                  <p class="eyebrow">Fuel, with and without shapes</p>
                  <div class="bar"><span class="lbl">with</span><span class="fill pine" :style="{ width: `${fuelSaved.ratio * 100}%` }"></span><span class="v">{{ fuelSaved.with }}</span></div>
                  <div class="bar"><span class="lbl">without</span><span class="fill" style="width: 100%"></span><span class="v">{{ fuelSaved.without }}</span></div>
                  <p class="muted small">Same value either way: a shape changes speed and fuel, never a result.</p>
                </div>
              </template>
            </div>

            <!-- 03 the evaluator -->
            <div v-else-if="stage === 'evaluator'">
              <p class="explain">The evaluator runs on fuel the host pushes. Here it was pushed one unit at a time, and after each the frames were read: scrub or play to watch it work.</p>
              <template v-if="result.compile.ok && steps.length">
                <div class="transport">
                  <button class="sjs-button" type="button" aria-label="Back one step" @click="move(-1)">◀</button>
                  <button class="sjs-button primary" type="button" @click="play">{{ playing ? 'Pause' : 'Play' }}</button>
                  <button class="sjs-button" type="button" aria-label="Forward one step" @click="move(1)">▶</button>
                  <input v-model.number="step" class="scrub" type="range" min="0" :max="steps.length - 1" aria-label="Step" @input="stop" />
                  <span class="counter">{{ steps[step]?.fuel ?? 0 }} / {{ result.fuel }} fuel</span>
                </div>
                <svg class="depth" viewBox="0 0 600 48" preserveAspectRatio="none" role="img" :aria-label="`Frame depth over ${steps.length} steps`">
                  <path :d="`${depthPath} L600,48 L0,48 Z`" class="area" />
                  <path :d="depthPath" class="line" />
                  <line :x1="cursorX" :x2="cursorX" y1="0" y2="48" class="cursor" />
                </svg>
                <p v-if="result.truncated" class="muted small">The trace shows the first {{ steps.length - 1 }} units; the evaluation ran on to {{ result.fuel }}.</p>
                <ol class="frames">
                  <li v-if="!frames.length" class="muted">{{ step === 0 ? 'Not started.' : result.status === 'fault' ? 'Faulted.' : 'Done: no frames left.' }}</li>
                  <li v-for="(f, i) in frames" :key="i" :class="[f.kind, { top: i === frames.length - 1 }]">
                    <span class="fname">{{ frameLabel(f, i) }}</span>
                    <span class="fat">{{ f.at.line }}:{{ f.at.column }}</span>
                    <span v-if="f.kind === 'loop' && f.length" class="prog">
                      <span class="track"><span class="fill" :style="{ width: `${(f.index / f.length) * 100}%` }"></span></span>
                      {{ f.index }} of {{ f.length }}
                    </span>
                  </li>
                </ol>
              </template>
              <p v-else class="muted">The program was refused before it could run.</p>
            </div>

            <!-- 04 the writer -->
            <div v-else-if="stage === 'writer'">
              <p class="explain">The writer writes the value as JavaScript source that evaluates to an equal value, into a buffer of any size: here {{ result.pieceSize }} bytes, so it stops and resumes.</p>
              <template v-if="result.output">
                <dl class="facts">
                  <div><dt>Kind</dt><dd>{{ result.kind }}</dd></div>
                  <div><dt>Pieces</dt><dd>{{ result.output.pieces }}</dd></div>
                  <div><dt>Fuel</dt><dd>{{ result.fuel }}</dd></div>
                </dl>
                <p class="output"><span v-for="(p, i) in pieces" :key="i" :class="`p${i % 2}`" :title="`piece ${i + 1}`">{{ p }}</span></p>
              </template>
              <p v-else class="muted">Nothing to write: the program did not give a value.</p>
            </div>
          </template>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.pipeline {
  --pane-border: var(--border-2);
  margin: var(--space-6) 0 var(--space-12);
}
.examples {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: var(--space-4);
}
.examples button {
  height: 30px;
  padding: 0 12px;
  font: var(--type-ui);
  color: var(--fg-2);
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-pill);
  cursor: pointer;
}
.examples button:hover {
  color: var(--fg-1);
  border-color: var(--border-strong);
}
.examples button.selected {
  color: var(--fg-on-accent);
  background: var(--accent);
  border-color: var(--accent);
}
.workbench {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: var(--space-4);
  align-items: start;
}
@media (max-width: 960px) {
  .workbench {
    grid-template-columns: minmax(0, 1fr);
  }
}
.panel {
  background: var(--surface-1);
  border: 1px solid var(--pane-border);
  border-radius: var(--radius-2);
}
.panel-head {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: 8px 12px;
  border-bottom: 1px solid var(--border-1);
}
.panel-head .eyebrow {
  margin: 0;
  flex: 1;
}
.toggle,
.select {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font: var(--type-label);
  letter-spacing: 0.04em;
  color: var(--fg-2);
}
.select select {
  font: var(--type-ui);
  color: var(--fg-1);
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  padding: 2px 4px;
}
.code-area {
  position: relative;
  background: var(--bg-grid), var(--surface-code);
}
.view,
.editor {
  margin: 0;
  padding: 14px 16px 14px 0;
  font: var(--type-code);
  font-size: 13.5px;
  line-height: 1.7;
  tab-size: 2;
  white-space: pre;
}
.view {
  min-height: 220px;
  overflow: hidden;
}
.editor {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  box-sizing: border-box;
  padding-left: 46px;
  border: 0;
  resize: none;
  overflow: auto;
  color: transparent;
  caret-color: var(--fg-1);
  background: transparent;
  outline: none;
}
.editor::selection {
  background: color-mix(in srgb, var(--accent) 25%, transparent);
  color: transparent;
}
.editor:focus {
  box-shadow: inset 0 0 0 2px var(--focus-ring);
}
.line {
  display: flex;
  white-space: pre;
}
.ln {
  flex: none;
  width: 46px;
  box-sizing: border-box;
  padding-right: 14px;
  text-align: right;
  color: var(--fg-4);
  user-select: none;
}
.text {
  color: var(--code-fg);
}
.text span {
  border-radius: 2px;
}
.text .c1 {
  background: color-mix(in srgb, var(--data-1) 22%, transparent);
}
.text .span {
  background: var(--accent-subtle);
}
.text .tok {
  box-shadow: inset 0 -2px 0 var(--accent);
  color: var(--accent-fg);
}
.text .outer {
  background: color-mix(in srgb, var(--data-2) 30%, transparent);
  box-shadow: inset 0 -2px 0 var(--data-2);
}
.text .now {
  background: var(--accent);
  color: var(--fg-on-accent);
}
.text .err {
  text-decoration: underline wavy var(--state-error);
  text-underline-offset: 4px;
  color: var(--state-error);
}
.legend {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 14px;
  margin: 0;
  padding: 8px 12px;
  border-top: 1px solid var(--border-1);
  font: var(--type-label);
  letter-spacing: 0.04em;
  color: var(--fg-3);
}
.legend:empty {
  display: none;
}
.sw {
  display: inline-block;
  width: 10px;
  height: 10px;
  margin: 0 5px -1px 0;
  border-radius: 2px;
}
.sw.c1 { background: color-mix(in srgb, var(--data-1) 40%, transparent); }
.sw.span { background: var(--accent-subtle); box-shadow: inset 0 -2px 0 var(--accent); }
.sw.now { background: var(--accent); }
.sw.outer { background: color-mix(in srgb, var(--data-2) 45%, transparent); margin-left: 6px; }
.sw.err { background: var(--state-error-bg); box-shadow: inset 0 -2px 0 var(--state-error); }

.stages {
  list-style: none;
  margin: 0 0 var(--space-3);
  padding: 0;
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 6px;
}
@media (max-width: 640px) {
  .stages {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
.stages li {
  margin: 0;
  position: relative;
}
.stage {
  display: grid;
  width: 100%;
  height: 100%;
  gap: 2px;
  padding: 10px 12px;
  text-align: left;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  cursor: pointer;
  position: relative;
}
.stage::before {
  content: '';
  position: absolute;
  inset: 0 auto 0 0;
  width: 3px;
  border-radius: 2px 0 0 2px;
  background: var(--state-running);
}
.stage.error::before { background: var(--state-error); }
.stage.idle::before { background: var(--border-2); }
.stage.idle { opacity: 0.6; }
.stage:hover { border-color: var(--border-strong); }
.stage.selected {
  border-color: var(--accent);
  box-shadow: 0 0 0 1px var(--accent);
}
.num {
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  color: var(--accent-fg);
}
.stage.error .num { color: var(--state-error); }
.name {
  font: var(--fw-regular) 20px/1.15 var(--font-serif);
  color: var(--fg-1);
}
.blurb {
  font-size: 12px;
  color: var(--fg-3);
}
.stat {
  margin-top: 6px;
  font-size: 12px;
  line-height: 1.4;
  color: var(--fg-2);
  font-variant-numeric: tabular-nums;
}
.detail {
  padding: var(--space-4);
  min-height: 320px;
}
.explain {
  margin: 0 0 var(--space-4);
  color: var(--fg-2);
  font-size: 14.5px;
  line-height: 1.6;
}
.muted { color: var(--fg-3); }
.small { font-size: 13px; }
.problem { color: var(--state-error); }
.error-box {
  display: flex;
  gap: var(--space-3);
  align-items: baseline;
  padding: 10px 12px;
  margin-bottom: var(--space-4);
  background: var(--state-error-bg);
  border-radius: var(--radius-2);
  color: var(--fg-1);
  font-size: 14px;
}
.events {
  list-style: none;
  margin: 0;
  padding: 0;
  max-height: 440px;
  overflow: auto;
  border: 1px solid var(--border-1);
  border-radius: var(--radius-2);
  font: var(--type-code);
  font-size: 12.5px;
}
.events li {
  display: grid;
  grid-template-columns: 34px minmax(120px, auto) 1fr auto;
  gap: 10px;
  align-items: baseline;
  margin: 0;
  padding: 3px 10px;
  border-bottom: 1px solid var(--border-1);
  cursor: default;
  outline: none;
}
.events li:nth-child(2n) {
  background: color-mix(in srgb, var(--surface-2) 50%, transparent);
}
.events li.on {
  background: var(--accent-subtle);
}
.idx { color: var(--fg-4); text-align: right; }
.kind { color: var(--accent-fg); white-space: nowrap; }
.pl { color: var(--fg-1); background: none !important; border: 0 !important; padding: 0 !important; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.pos { color: var(--fg-3); white-space: nowrap; font-size: 11.5px; }
@media (max-width: 640px) {
  .events li { grid-template-columns: 28px auto 1fr; }
  .pos { display: none; }
}
.facts {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
  margin: 0 0 var(--space-4);
}
.facts div {
  padding: 10px 12px;
  border: 1px solid var(--border-1);
  border-radius: var(--radius-2);
  background: var(--bg-app);
}
.facts dt {
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  text-transform: uppercase;
  color: var(--fg-3);
}
.facts dd {
  margin: 4px 0 0;
  font: var(--fw-regular) 22px/1.1 var(--font-serif);
  color: var(--fg-1);
}
.fns {
  display: table;
  width: 100%;
  margin: 0 0 var(--space-4);
  border-collapse: collapse;
  font-size: 13.5px;
}
.fns th {
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  text-transform: uppercase;
  color: var(--fg-3);
  text-align: left;
  padding: 6px 8px;
  border-bottom: 1px solid var(--border-2);
}
.fns td {
  padding: 6px 8px;
  border-bottom: 1px solid var(--border-1);
  font-family: var(--font-mono);
  font-size: 12.5px;
}
.fns tr.on td { background: var(--accent-subtle); }
.bars .bar {
  display: grid;
  grid-template-columns: 64px 1fr 56px;
  gap: 10px;
  align-items: center;
  margin: 6px 0;
  font: var(--type-label);
  color: var(--fg-2);
}
.bars .fill {
  display: block;
  height: 12px;
  border-radius: 2px;
  background: var(--border-2);
  grid-column: 2;
}
.bars .fill.pine { background: var(--accent); }
.bars .v { text-align: right; color: var(--fg-1); }
.bars .eyebrow { margin-top: var(--space-2); }
.transport {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  margin-bottom: var(--space-3);
}
.scrub {
  flex: 1;
  min-width: 140px;
  accent-color: var(--accent);
}
.counter {
  font: var(--type-label);
  color: var(--fg-2);
  min-width: 110px;
  text-align: right;
}
.depth {
  display: block;
  width: 100%;
  height: 48px;
  margin-bottom: var(--space-3);
  background: var(--bg-grid), var(--bg-app);
  border: 1px solid var(--border-1);
  border-radius: var(--radius-2);
}
.depth .area { fill: color-mix(in srgb, var(--accent) 14%, transparent); }
.depth .line { fill: none; stroke: var(--accent); stroke-width: 1.5; vector-effect: non-scaling-stroke; }
.depth .cursor { stroke: var(--state-waiting); stroke-width: 2; vector-effect: non-scaling-stroke; }
.frames {
  list-style: none;
  margin: 0;
  padding: 0;
  display: grid;
  gap: 4px;
}
.frames li {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 4px 12px;
  margin: 0;
  padding: 8px 12px;
  border: 1px solid var(--border-1);
  border-radius: var(--radius-2);
  background: var(--bg-app);
  font-size: 13.5px;
}
.frames li.loop { border-left: 3px solid var(--data-2); }
.frames li.function { border-left: 3px solid var(--border-2); }
.frames li.top { border-color: var(--accent); border-left-color: var(--accent); background: var(--accent-subtle); }
.fname { font-family: var(--font-mono); font-weight: 500; color: var(--fg-1); }
.fat { font: var(--type-label); color: var(--fg-3); }
.prog {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  margin-left: auto;
  font: var(--type-label);
  color: var(--fg-2);
}
.track {
  display: inline-block;
  width: 90px;
  height: 6px;
  border-radius: 3px;
  background: var(--border-1);
  overflow: hidden;
}
.track .fill {
  display: block;
  height: 100%;
  background: var(--accent);
}
.output {
  margin: 0;
  padding: 14px 16px;
  font: var(--type-code);
  background: var(--surface-code);
  border: 1px solid var(--code-border);
  border-radius: var(--radius-2);
  white-space: pre-wrap;
  word-break: break-all;
}
.output .p1 {
  background: color-mix(in srgb, var(--data-1) 25%, transparent);
}
.output span {
  border-radius: 2px;
}
</style>
