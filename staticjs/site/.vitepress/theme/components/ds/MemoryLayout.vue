<script setup lang="ts">
// A struct's bytes, field by field, on rows. A port of the Tinyactors design
// system's MemoryLayout (tinyactors-dev/website, components/memory/
// MemoryLayout.jsx), without its cache-line warnings and table.
import { computed } from 'vue';

export interface Field { name: string; type?: string; size: number; kind?: keyof typeof FIELD_KINDS; note?: string; align?: number }

const props = withDefaults(defineProps<{ name?: string; fields: Field[]; bytesPerRow?: number; rowHeight?: number; legend?: boolean }>(), {
  bytesPerRow: 16,
  rowHeight: 44,
  legend: true,
});

const HATCH = 'repeating-linear-gradient(135deg, var(--border-2) 0 1px, transparent 1px 5px), var(--surface-1)';
const FIELD_KINDS = {
  data: { bg: 'var(--surface-1)', fg: 'var(--fg-1)', legend: 'data' },
  hot: { bg: 'var(--state-running-bg)', fg: 'var(--state-running)', legend: 'hot path' },
  warm: { bg: 'var(--state-waiting-bg)', fg: 'var(--state-waiting)', legend: 'warm' },
  cold: { bg: 'var(--surface-2)', fg: 'var(--fg-2)', legend: 'cold' },
  ptr: { bg: 'color-mix(in srgb, var(--data-4) 26%, var(--surface-1))', fg: 'var(--fg-1)', legend: 'number or pointer' },
  atomic: { bg: 'var(--state-done-bg)', fg: 'var(--state-done)', legend: 'atomic' },
  pad: { bg: HATCH, fg: 'var(--fg-4)', legend: 'padding' },
};
const natural = (s: number) => ([1, 2, 4, 8].includes(s) ? s : s % 8 === 0 ? 8 : 1);

const laid = computed(() => {
  let off = 0, maxA = 1;
  const out: (Field & { offset: number })[] = [];
  for (const f of props.fields) {
    const a = f.align ?? natural(f.size);
    maxA = Math.max(maxA, a);
    const at = Math.ceil(off / a) * a;
    if (at > off) out.push({ kind: 'pad', size: at - off, offset: off, name: '' });
    out.push({ kind: 'data', ...f, offset: at });
    off = at + f.size;
  }
  const size = Math.ceil(off / maxA) * maxA;
  if (size > off) out.push({ kind: 'pad', size: size - off, offset: off, name: '' });
  return { fields: out, size };
});
const rows = computed(() => Math.max(1, Math.ceil(laid.value.size / props.bytesPerRow)));
const segs = computed(() => {
  const out: { f: Field & { offset: number }; r: number; col: number; span: number; first: boolean; last: boolean }[] = [];
  for (const f of laid.value.fields) {
    let s = f.offset; const e = f.offset + f.size;
    while (s < e) {
      const r = Math.floor(s / props.bytesPerRow), se = Math.min(e, (r + 1) * props.bytesPerRow);
      out.push({ f, r, col: s % props.bytesPerRow, span: se - s, first: s === f.offset, last: se === e });
      s = se;
    }
  }
  return out;
});
const kinds = computed(() => [...new Set(laid.value.fields.map((f) => f.kind || 'data'))].filter((k) => k !== 'data' && k !== 'pad'));
const tick = computed(() => (props.bytesPerRow >= 32 ? 8 : 4));
const kind = (f: Field) => FIELD_KINDS[f.kind || 'data'] || FIELD_KINDS.data;
</script>

<template>
  <div class="ta-mem">
    <div v-if="name" class="title">{{ name }}</div>
    <div class="grid">
      <span />
      <div class="scale">
        <span v-for="i in bytesPerRow / tick" :key="i" :style="{ left: `${(((i - 1) * tick) / bytesPerRow) * 100}%` }">{{ (i - 1) * tick }}</span>
      </div>
      <template v-for="r in rows" :key="r">
        <div class="offset">+{{ (r - 1) * bytesPerRow }}</div>
        <div class="row" :style="{ gridTemplateColumns: `repeat(${bytesPerRow}, minmax(0,1fr))`, height: `${rowHeight}px`, background: `repeating-linear-gradient(to right, transparent 0 calc(100% / ${bytesPerRow} - 1px), var(--border-1) calc(100% / ${bytesPerRow} - 1px) calc(100% / ${bytesPerRow}))` }">
          <div
            v-for="(s, i) in segs.filter((x) => x.r === r - 1)"
            :key="i"
            class="field"
            :title="s.f.kind === 'pad' ? `${s.f.size} B padding at +${s.f.offset}` : `${s.f.type ? s.f.type + ' ' : ''}${s.f.name} · ${s.f.size} B at +${s.f.offset}${s.f.note ? ' — ' + s.f.note : ''}`"
            :style="{ gridColumn: `${s.col + 1} / span ${s.span}`, background: kind(s.f).bg, boxShadow: s.last ? 'inset -1px 0 0 var(--fg-1)' : 'none' }"
          >
            <span v-if="s.f.kind !== 'pad'" :style="{ color: s.first ? kind(s.f).fg : 'var(--fg-3)' }">{{ s.f.name }}</span>
            <span v-if="s.f.kind !== 'pad' && s.f.type && s.first" class="type">{{ s.f.type }}</span>
          </div>
        </div>
      </template>
    </div>
    <div class="summary"><strong>sizeof = {{ laid.size }} B</strong></div>
    <div v-if="legend && kinds.length" class="legend">
      <span v-for="k in kinds" :key="k"><i :style="{ background: FIELD_KINDS[k].bg }" />{{ FIELD_KINDS[k].legend }}</span>
    </div>
  </div>
</template>

<style scoped>
.ta-mem {
  min-width: 0;
  color: var(--fg-1);
}
.title {
  font: 500 13px var(--font-mono);
  margin-bottom: 8px;
  white-space: pre;
}
.grid {
  display: grid;
  grid-template-columns: 44px minmax(0, 1fr);
  row-gap: 6px;
}
.scale {
  position: relative;
  height: 12px;
}
.scale span {
  position: absolute;
  bottom: 0;
  height: 12px;
  display: flex;
  align-items: flex-end;
  padding-left: 2px;
  border-left: 1px solid var(--border-2);
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-4);
}
.offset {
  display: flex;
  align-items: center;
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-3);
}
.row {
  display: grid;
  border: 1.25px solid var(--fg-1);
}
.field {
  min-width: 0;
  padding: 5px 6px;
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: 3px;
  overflow: hidden;
}
.field span {
  font: 500 11px/1 var(--font-mono);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.field .type {
  font-weight: 400;
  font-size: 10px;
  color: var(--fg-3);
}
.summary {
  margin-top: 10px;
  padding-left: 44px;
  font: 400 11px/1.3 var(--font-mono);
  color: var(--fg-2);
}
.summary strong {
  color: var(--fg-1);
  font-weight: 500;
}
.legend {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 14px;
  margin-top: 8px;
  padding-left: 44px;
  font: 400 11px/1 var(--font-mono);
  color: var(--fg-3);
}
.legend span {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  white-space: nowrap;
}
.legend i {
  width: 12px;
  height: 12px;
  border: 1px solid var(--fg-1);
}
</style>
