<script setup lang="ts">
// The page's staticjs in its one linear memory, read live: the module's
// `memory.buffer` drawn to scale (the C stack, the module's data, the heap),
// the heap enlarged with where the context, the evaluator, the
// program and the result's object are, and the result's 16-byte sjs_value as
// it lies in the buffer, byte by byte. Each choice runs its program in this
// page and reads the memory again (sjs_site_memory).
import { computed, onMounted, ref } from 'vue';
import { useStaticJS } from '../staticjs/client.js';

const props = defineProps<{ programs: string[] }>();

interface Memory {
  stackLow: number; stackHigh: number; dataStart: number; dataEnd: number; heapBase: number;
  session: number; context: number; vm: number; program: number; result: number; resultObject: number;
  liveBytes: number; pages: number; size: number; resultBytes: number[] | null;
}

const chosen = ref(0);
const memory = ref<Memory | null>(null);
const kind = ref('');
const value = ref('');
const failed = ref('');

const PAGE = 65536;
const tint = (n: number) => `color-mix(in oklch, var(--data-${n}) 38%, var(--surface-1))`;
const hex = (n: number, width = 8) => `0x${n.toString(16).padStart(width, '0')}`;
const bytes = (n: number) => n.toLocaleString('en-US');
const size = (n: number) => (n >= 1024 * 1024 ? `${+(n / 1048576).toFixed(2)} MiB` : n >= 1024 ? `${+(n / 1024).toFixed(1)} KiB` : `${n} B`);

async function show(index: number) {
  chosen.value = index;
  try {
    const staticjs = await useStaticJS();
    const r = staticjs.evaluate(props.programs[index]);
    if (!r.ok) throw new Error(r.error?.message ?? 'the program did not run');
    kind.value = r.kind;
    value.value = r.value;
    memory.value = staticjs.memory();
    failed.value = '';
  } catch (error) {
    failed.value = String((error as Error).message ?? error);
  }
}
onMounted(() => show(0));

/** The buffer's regions, in address order, as fractions of its size. */
const regions = computed(() => {
  const m = memory.value;
  if (!m) return [];
  const all = [
    { name: 'C stack', from: m.stackLow, to: m.stackHigh, color: 'var(--surface-2)', note: 'grows down' },
    { name: 'data', from: m.dataStart, to: m.dataEnd, color: tint(2), note: 'built-ins, constants' },
    { name: 'heap', from: m.heapBase, to: m.size, color: tint(4), note: 'blocks from the allocator' },
  ];
  return all.filter((r) => r.to > r.from).map((r) => ({ ...r, left: (r.from / m.size) * 100, width: ((r.to - r.from) / m.size) * 100 }));
});

/** What the heap holds that the page knows the address of. */
const pins = computed(() => {
  const m = memory.value;
  if (!m) return { from: 0, to: 0, items: [] as { name: string; at: number; left: number; lane: number }[] };
  const raw = [
    { name: 'context', at: m.context },
    { name: 'evaluator', at: m.vm },
    { name: 'program', at: m.program },
    ...(m.result ? [{ name: 'result', at: m.result }] : []),
    ...(m.resultObject ? [{ name: 'result’s object', at: m.resultObject }] : []),
  ].filter((p) => p.at >= m.heapBase);
  // The enlarged part: from the heap's base to a page past the last pin.
  const last = Math.max(...raw.map((p) => p.at));
  const to = Math.min(m.size, (Math.floor((last - m.heapBase) / PAGE) + 1) * PAGE + m.heapBase);
  const sorted = raw.sort((a, b) => a.at - b.at).map((p) => ({ ...p, left: ((p.at - m.heapBase) / (to - m.heapBase)) * 100, lane: 0 }));
  // Labels too close to the one before them go to the next lane down.
  const lanes: number[] = [];
  for (const p of sorted) {
    let lane = 0;
    while (lanes[lane] !== undefined && p.left - lanes[lane] < 16) lane++;
    lanes[lane] = p.left;
    p.lane = lane;
  }
  return { from: m.heapBase, to, items: sorted };
});

/** The result's sjs_value, byte by byte, tinted by field. */
const FIELDS = [
  { name: 'kind', from: 0, size: 1, color: 1 },
  { name: 'internal', from: 1, size: 1, color: 0 },
  { name: 'reserved', from: 2, size: 2, color: 0 },
  { name: 'boolean', from: 4, size: 4, color: 3 },
  { name: 'as', from: 8, size: 8, color: 4 },
];
const cells = computed(() => {
  const b = memory.value?.resultBytes;
  if (!b) return [];
  return b.map((byte, i) => {
    const f = FIELDS.find((x) => i >= x.from && i < x.from + x.size)!;
    return { text: byte.toString(16).padStart(2, '0'), field: f.name, color: f.color ? tint(f.color) : 'var(--surface-1)' };
  });
});
const asText = computed(() => {
  const m = memory.value;
  if (!m?.resultBytes) return '';
  if (m.resultObject) return `${hex(m.resultObject)}, the address of the ${kind.value}’s object in the heap`;
  if (kind.value === 'number') return `${new DataView(new Uint8Array(m.resultBytes).buffer).getFloat64(8, true)}, the number itself`;
  return 'unused';
});
const fieldValues = computed(() => {
  const b = memory.value?.resultBytes;
  if (!b) return [];
  return [
    { name: 'kind', color: 1, text: `${b[0]}, ${kind.value}` },
    { name: 'boolean', color: 3, text: kind.value === 'boolean' ? `${b[4]}, ${b[4] ? 'true' : 'false'}` : 'unused' },
    { name: 'as', color: 4, text: asText.value },
  ];
});
</script>

<template>
  <div class="memory-map">
    <div class="programs" role="group" aria-label="Program">
      <button v-for="(p, i) in programs" :key="p" type="button" :class="{ on: i === chosen }" :aria-pressed="i === chosen" @click="show(i)">
        <code>{{ p }}</code>
      </button>
    </div>
    <p v-if="failed" class="failed">{{ failed }}</p>
    <template v-if="memory">
      <div class="buffer-title">
        <code>memory.buffer</code>
        <span>{{ bytes(memory.size) }} bytes, {{ memory.pages }} pages of 64 KiB</span>
      </div>
      <div class="strip" aria-hidden="true">
        <i v-for="r in regions" :key="r.name" :style="{ left: `${r.left}%`, width: `${r.width}%`, background: r.color }" :title="`${r.name}: ${hex(r.from)} to ${hex(r.to)}`" />
        <span class="pages" :style="{ backgroundSize: `${(PAGE / memory.size) * 100}% 100%` }" />
      </div>
      <div class="ruler"><span>0</span><span>{{ bytes(memory.size) }}</span></div>
      <ul class="regions">
        <li v-for="r in regions" :key="r.name">
          <i :style="{ background: r.color }" />
          <strong>{{ r.name }}</strong>
          <span>{{ hex(r.from) }} to {{ hex(r.to) }}, {{ size(r.to - r.from) }}, {{ r.note }}</span>
        </li>
      </ul>

      <div class="zoom-title">
        <span>The heap’s first {{ size(pins.to - pins.from) }}, from {{ hex(pins.from) }}</span>
        <span>{{ bytes(memory.liveBytes) }} bytes live in the context</span>
      </div>
      <div class="strip heap" :style="{ background: tint(4) }" aria-hidden="true">
        <b v-for="p in pins.items" :key="p.name" :style="{ left: `${p.left}%` }" />
      </div>
      <div class="pins" :style="{ height: `${(Math.max(0, ...pins.items.map((p) => p.lane)) + 1) * 34}px` }">
        <span v-for="p in pins.items" :key="p.name" :class="{ end: p.left > 70 }" :style="{ left: `${p.left}%`, top: `${p.lane * 34}px` }">
          <strong>{{ p.name }}</strong><code>{{ hex(p.at) }}</code>
        </span>
      </div>

      <div class="value-title">
        <code>new Uint8Array(memory.buffer, {{ memory.result }}, 16)</code>
        <span>the result, <code>{{ value }}</code>, as an <code>sjs_value</code> at {{ hex(memory.result) }}</span>
      </div>
      <div class="hex">
        <span class="offset">+0</span>
        <i v-for="(c, i) in cells.slice(0, 8)" :key="i" :style="{ background: c.color }" :title="`${c.field}, byte ${i}`">{{ c.text }}</i>
        <span class="offset">+8</span>
        <i v-for="(c, i) in cells.slice(8)" :key="i + 8" :style="{ background: c.color }" :title="`${c.field}, byte ${i + 8}`">{{ c.text }}</i>
      </div>
      <ul class="fields">
        <li v-for="f in fieldValues" :key="f.name"><i :style="{ background: tint(f.color) }" /><code>{{ f.name }}</code> {{ f.text }}</li>
      </ul>
    </template>
  </div>
</template>

<style scoped>
.memory-map {
  min-width: 0;
  color: var(--fg-1);
}
.programs {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 20px;
}
.programs button {
  border: 1px solid var(--border-2);
  background: var(--surface-1);
  padding: 6px 10px;
  border-radius: 0;
  cursor: pointer;
  color: var(--fg-2);
}
.programs button.on {
  border-color: var(--fg-1);
  color: var(--fg-1);
  box-shadow: inset 0 -2px 0 var(--accent-fg);
}
.programs code {
  font: 400 12px/1.3 var(--font-mono);
  background: none;
  padding: 0;
}
.failed {
  color: var(--state-error);
  font: 400 13px var(--font-mono);
}
.buffer-title,
.zoom-title,
.value-title {
  display: flex;
  flex-wrap: wrap;
  justify-content: space-between;
  gap: 4px 16px;
  font: 400 12px/1.4 var(--font-mono);
  color: var(--fg-3);
  margin-bottom: 6px;
}
.buffer-title code,
.value-title > code {
  font: 500 13px var(--font-mono);
  color: var(--fg-1);
  background: none;
  padding: 0;
}
.value-title span code {
  font-size: 12px;
}
.zoom-title,
.value-title {
  margin-top: 28px;
}
.strip {
  position: relative;
  height: 36px;
  border: 1.25px solid var(--fg-1);
  background: var(--surface-1);
  overflow: hidden;
}
.strip i {
  position: absolute;
  top: 0;
  bottom: 0;
  border-right: 1px solid var(--fg-1);
}
.strip .pages {
  position: absolute;
  inset: 0;
  background-image: linear-gradient(to right, transparent calc(100% - 1px), var(--border-2) calc(100% - 1px));
  pointer-events: none;
}
.strip.heap {
  overflow: visible;
}
.strip.heap b {
  position: absolute;
  top: -1px;
  bottom: -8px;
  width: 2px;
  margin-left: -1px;
  background: var(--fg-1);
}
.ruler {
  display: flex;
  justify-content: space-between;
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-4);
  margin-top: 4px;
}
.regions,
.fields {
  list-style: none;
  padding: 0;
  margin: 10px 0 0;
  display: grid;
  gap: 4px;
  font: 400 12px/1.5 var(--font-mono);
  color: var(--fg-2);
}
.regions li,
.fields li {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: 0 8px;
  margin: 0;
}
.regions i,
.fields i {
  width: 10px;
  height: 10px;
  border: 1px solid var(--fg-1);
  flex: none;
  align-self: center;
}
.regions strong {
  font-weight: 500;
  color: var(--fg-1);
  min-width: 64px;
}
.fields code {
  background: none;
  padding: 0;
  color: var(--fg-1);
  min-width: 64px;
}
.pins {
  position: relative;
  margin-top: 10px;
}
.pins span {
  position: absolute;
  display: grid;
  transform: translateX(-2px);
  padding-left: 6px;
  border-left: 1px solid var(--border-2);
  font: 400 11px/1.3 var(--font-mono);
  white-space: nowrap;
}
.pins span.end {
  transform: translateX(calc(-100% + 1px));
  padding: 0 6px 0 0;
  border-left: 0;
  border-right: 1px solid var(--border-2);
  text-align: right;
}
.pins strong {
  font-weight: 500;
}
.pins code {
  background: none;
  padding: 0;
  font-size: 10px;
  color: var(--fg-3);
}
.hex {
  display: grid;
  grid-template-columns: 32px repeat(8, minmax(0, 40px));
  width: 100%;
  max-width: 352px;
}
.hex .offset {
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-3);
  display: flex;
  align-items: center;
}
.hex i {
  height: 32px;
  display: flex;
  align-items: center;
  justify-content: center;
  font: 500 12px/1 var(--font-mono);
  font-style: normal;
  border: 1px solid var(--border-2);
  margin: 0 -1px -1px 0;
}
</style>
