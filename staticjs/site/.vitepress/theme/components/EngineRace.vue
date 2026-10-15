<script setup lang="ts">
// The front page's race: staticjs and QuickJS-NG, both compiled to
// WebAssembly, run the same programs in this browser, in a worker. A
// program's time is shown only once both engines gave the same result.
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue';
import { withBase } from 'vitepress';
import { raceCases } from '../staticjs/race-cases.js';
import Icon from './Icon.vue';

type Times = { staticjs: { ns: number; runs: number }; quickjs: { ns: number; runs: number } };
type Row = { state: 'waiting' | 'running' | 'done'; value?: string; same?: boolean; times?: Times | null };

const rows = reactive<Record<string, Row>>(Object.fromEntries(raceCases.map((c) => [c.id, { state: 'waiting' }])));
const phase = ref<'idle' | 'loading' | 'running' | 'done' | 'error'>('idle');
const message = ref('');
const startup = ref<any>(null);
const open = ref<string | null>(null);
const root = ref<HTMLElement | null>(null);
let worker: Worker | null = null;
let observer: IntersectionObserver | null = null;

function reset() {
  for (const c of raceCases) rows[c.id] = { state: 'waiting' };
}

function start() {
  if (phase.value === 'loading' || phase.value === 'running') return;
  reset();
  worker?.terminate();
  worker = new Worker(new URL('../staticjs/race.worker.js', import.meta.url), { type: 'module' });
  phase.value = 'loading';
  worker.onmessage = ({ data }) => {
    if (data.type === 'ready') {
      startup.value = data.startup;
      phase.value = 'running';
    } else if (data.type === 'running') {
      rows[data.id] = { state: 'running' };
    } else if (data.type === 'case') {
      rows[data.id] = { state: 'done', value: data.value, same: data.same, times: data.times };
    } else if (data.type === 'done') {
      phase.value = 'done';
      worker?.terminate();
      worker = null;
    } else if (data.type === 'error') {
      phase.value = 'error';
      message.value = data.message;
    }
  };
  worker.postMessage({ type: 'run', staticjsURL: new URL(withBase('/staticjs-site.wasm'), location.href).href });
}

onMounted(() => {
  // The race starts when it comes into view: it keeps a CPU busy for a few seconds.
  observer = new IntersectionObserver((entries) => {
    if (entries.some((e) => e.isIntersecting)) {
      observer?.disconnect();
      start();
    }
  }, { threshold: 0.25 });
  if (root.value) observer.observe(root.value);
});
onBeforeUnmount(() => {
  observer?.disconnect();
  worker?.terminate();
});

const finished = computed(() => raceCases.filter((c) => rows[c.id].times));
const faster = computed(() => finished.value.filter((c) => rows[c.id].times!.quickjs.ns >= 1.05 * rows[c.id].times!.staticjs.ns).length);
const status = computed(() => {
  if (phase.value === 'idle') return { kind: 'idle', text: 'Waiting to start.' };
  if (phase.value === 'loading') return { kind: 'busy', text: 'Loading both engines.' };
  if (phase.value === 'error') return { kind: 'error', text: `The race stopped: ${message.value}` };
  const now = raceCases.find((c) => rows[c.id].state === 'running');
  if (phase.value === 'running') return { kind: 'busy', text: now ? `Running ${now.name.toLowerCase()}.` : 'Running.' };
  return { kind: 'running', text: `staticjs was faster on ${faster.value} of ${finished.value.length} programs.` };
});

function time(ns: number) {
  if (ns < 1e3) return `${ns.toFixed(0)} ns`;
  if (ns < 1e6) return `${(ns / 1e3).toFixed(ns < 1e4 ? 2 : 1)} µs`;
  return `${(ns / 1e6).toFixed(2)} ms`;
}
function ratio(t: Times) {
  const r = t.quickjs.ns / t.staticjs.ns;
  if (r > 0.95 && r < 1.05) return 'about even';
  return r >= 1 ? `${r.toFixed(1)}× faster` : `${(1 / r).toFixed(1)}× slower`;
}
function width(t: Times, engine: 'staticjs' | 'quickjs') {
  return `${(t[engine].ns / Math.max(t.staticjs.ns, t.quickjs.ns)) * 100}%`;
}
const running = computed(() => phase.value === 'loading' || phase.value === 'running');
</script>

<template>
  <div ref="root" class="race" aria-labelledby="race-title">
    <div class="head">
      <div>
        <h3 id="race-title" class="race-title">Live measurement</h3>
        <p class="lede">
          staticjs and QuickJS-NG, both compiled to WebAssembly, run eight programs in a web worker, and a row is timed
          only when both engines return the same result.
        </p>
      </div>
      <button class="sjs-button big" :class="{ primary: phase !== 'done' }" type="button" :disabled="running" @click="start">
        <Icon :name="phase === 'done' || phase === 'error' ? 'reset' : 'play'" />
        {{ phase === 'done' || phase === 'error' ? 'Run again' : 'Run the comparison' }}
      </button>
    </div>

    <p class="status" :data-kind="status.kind" aria-live="polite">{{ status.text }}</p>

    <div class="race-table card" role="table" aria-label="Time per run of each program">
      <div class="row header" role="row">
        <span role="columnheader">Program</span>
        <span role="columnheader" class="num">staticjs</span>
        <span role="columnheader" class="num">QuickJS-NG</span>
        <span role="columnheader" class="legend">
          <span><i class="swatch sjs"></i>staticjs</span>
          <span><i class="swatch other"></i>QuickJS-NG</span>
        </span>
      </div>
      <template v-for="c in raceCases" :key="c.id">
        <div class="row" role="row" :class="[rows[c.id].state, { open: open === c.id }]">
          <span role="rowheader" class="program">
            <button type="button" :aria-expanded="open === c.id" @click="open = open === c.id ? null : c.id">
              {{ c.name }}
            </button>
            <span class="note">{{ c.note }}</span>
          </span>
          <span role="cell" class="num sjs">{{ rows[c.id].times ? time(rows[c.id].times!.staticjs.ns) : '' }}</span>
          <span role="cell" class="num other">{{ rows[c.id].times ? time(rows[c.id].times!.quickjs.ns) : '' }}</span>
          <span role="cell" class="bars">
            <template v-if="rows[c.id].times">
              <span class="bar"><span class="fill sjs" :style="{ width: width(rows[c.id].times!, 'staticjs') }"></span></span>
              <span class="bar"><span class="fill other" :style="{ width: width(rows[c.id].times!, 'quickjs') }"></span></span>
              <span class="verdict">{{ ratio(rows[c.id].times!) }}</span>
            </template>
            <span v-else-if="rows[c.id].state === 'done'" class="status" data-kind="error">The engines returned different results, so this row has no time.</span>
            <span v-else-if="rows[c.id].state === 'running'" class="status" data-kind="busy">running</span>
          </span>
        </div>
        <div v-if="open === c.id" class="row detail" role="row">
          <div role="cell">
            <pre class="source"><code>{{ c.source }}</code></pre>
            <p v-if="rows[c.id].value" class="gives">
              <Icon :name="rows[c.id].same ? 'check' : 'x'" :size="14" />
              {{ rows[c.id].same ? 'Both give' : 'staticjs gives' }} <code>{{ rows[c.id].value }}</code>
            </p>
          </div>
        </div>
      </template>
    </div>

    <p class="note">
      Each time is the median of five samples of at least 40 ms. staticjs is called from JavaScript once per run, as a
      host calls the C API. QuickJS-NG (quickjs-wasi{{ startup ? ` ${startup.quickjs.version}` : '' }}) loops inside its
      engine and is called once per sample.
      <template v-if="startup">
        Starting from a compiled module takes {{ startup.staticjs.ms.toFixed(2) }} ms for staticjs and
        {{ startup.quickjs.ms.toFixed(2) }} ms for QuickJS-NG.
      </template>
    </p>
  </div>
</template>

<style scoped>
.race {
  margin-top: var(--space-10);
}
.race-title {
  font: var(--type-h3);
  font-size: 20px;
  color: var(--fg-1);
  margin: 0 0 var(--space-2);
}
.head {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-end;
  justify-content: space-between;
  gap: var(--space-4) var(--space-8);
}
.head .lede {
  margin-bottom: var(--space-4);
}
.head .sjs-button {
  margin-bottom: var(--space-4);
}
.race > .status {
  margin: 0 0 var(--space-3);
}
.race-table {
  overflow: hidden;
}
.row {
  display: grid;
  grid-template-columns: minmax(0, 1.4fr) 6.5em 6.5em minmax(0, 1.6fr);
  gap: var(--space-2) var(--space-4);
  align-items: center;
  padding: var(--space-3) var(--space-4);
  border-top: 1px solid var(--border-1);
}
.row.header .num {
  font: var(--type-ui);
}
.legend {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-1) var(--space-4);
  font-weight: 400;
  color: var(--fg-2);
}
.swatch {
  display: inline-block;
  width: 10px;
  height: 8px;
  margin-right: 6px;
  border-radius: 2px;
}
.swatch.sjs {
  background: var(--accent);
}
.swatch.other {
  background: var(--fg-3);
}
.row.header {
  border-top: 0;
  background: var(--surface-2);
  font: var(--type-ui);
  color: var(--fg-1);
}
.row.waiting .program,
.row.waiting .num {
  color: var(--fg-3);
}
.row.running {
  background: color-mix(in srgb, var(--state-waiting-bg) 50%, transparent);
}
.program {
  display: flex;
  flex-direction: column;
  min-width: 0;
}
.program button {
  align-self: flex-start;
  padding: 0;
  font: var(--type-ui);
  font-size: var(--fs-14);
  color: var(--fg-1);
  background: none;
  border: 0;
  border-bottom: 1px dotted var(--border-strong);
  cursor: pointer;
  text-align: left;
}
.program button:hover {
  color: var(--accent-fg);
  border-bottom-color: var(--accent);
}
.program .note {
  font: var(--type-body-sm);
  font-size: 12.5px;
  color: var(--fg-3);
}
.num {
  font: var(--type-code);
  text-align: right;
  white-space: nowrap;
}
.num.sjs {
  color: var(--accent-fg);
  font-weight: 500;
}
.num.other {
  color: var(--fg-2);
}
.bars {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  grid-template-rows: auto auto;
  column-gap: var(--space-3);
  row-gap: 3px;
  align-items: center;
}
.bar {
  grid-column: 1;
  height: 8px;
  background: var(--surface-2);
  border-radius: 2px;
  overflow: hidden;
}
.fill {
  display: block;
  height: 100%;
  border-radius: 2px;
  transition: width var(--dur-base, 240ms) var(--ease-step, ease-out);
}
.fill.sjs {
  background: var(--accent);
}
.fill.other {
  background: var(--fg-3);
}
.verdict {
  grid-column: 2;
  grid-row: 1 / span 2;
  font: var(--type-ui);
  font-size: 12.5px;
  color: var(--fg-1);
  white-space: nowrap;
}
.detail {
  display: block;
  background: var(--surface-code);
}
.source {
  margin: 0;
  padding: 0;
  font: var(--type-code);
  font-size: 12.5px;
  color: var(--code-fg);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
.gives {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  margin: var(--space-2) 0 0;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.gives code {
  font: var(--type-code);
  font-size: 12.5px;
  color: var(--fg-1);
  overflow-wrap: anywhere;
}
.race > .note {
  margin: var(--space-3) 0 0;
  max-width: var(--prose-max);
  font: var(--type-body-sm);
  color: var(--fg-3);
}
@media (max-width: 720px) {
  .row {
    grid-template-columns: 1fr 1fr;
  }
  .row.header span:nth-child(4) {
    display: none;
  }
  .program,
  .bars {
    grid-column: 1 / -1;
  }
  .num {
    text-align: left;
  }
  .num.sjs::before {
    content: 'staticjs ';
    color: var(--fg-3);
    font-weight: 400;
  }
  .num.other::before {
    content: 'QuickJS ';
    color: var(--fg-3);
  }
  .row.header {
    display: none;
  }
  .num:empty {
    display: none;
  }
}
</style>
