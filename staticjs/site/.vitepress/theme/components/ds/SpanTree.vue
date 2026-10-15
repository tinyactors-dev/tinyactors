<script setup lang="ts">
// A trace waterfall: spans as rows, nested by parent, with bars on a time
// axis. A port of the Tinyactors design system's SpanTree (tinyactors-dev/
// website, components/observability/SpanTree.jsx). `formatTime` replaces its
// milliseconds, so the axis can be any unit (here, fuel).
import { computed, ref } from 'vue';
import Icon from '../Icon.vue';

export interface Span { spanId: string; parentId?: string; name: string; service: string; start: number; duration: number; status?: 'error'; events?: { name: string; time: number }[] }
type Node = Span & { children: Node[] };

const props = withDefaults(
  defineProps<{ spans: Span[]; selectedId?: string; serviceColors?: Record<string, string>; treeWidth?: number; rowHeight?: number; ticks?: number; showRuler?: boolean; formatTime?: (t: number) => string }>(),
  { serviceColors: () => ({}), treeWidth: 320, rowHeight: 26, ticks: 4, showRuler: true, formatTime: (t: number) => `${t}` },
);
const emit = defineEmits<{ select: [id: string, span: Span] }>();

const PALETTE = ['var(--data-1)', 'var(--data-2)', 'var(--data-3)', 'var(--data-4)', 'var(--data-5)', 'var(--data-6)'];
const hash = (s: string) => { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return Math.abs(h); };
const color = (svc: string) => props.serviceColors[svc] || PALETTE[hash(svc || '') % PALETTE.length];
const collapsed = ref(new Set<string>());
const toggle = (id: string) => { const n = new Set(collapsed.value); n.has(id) ? n.delete(id) : n.add(id); collapsed.value = n; };

const roots = computed(() => {
  const byID = new Map<string, Node>(props.spans.map((s) => [s.spanId, { ...s, children: [] }]));
  const out: Node[] = [];
  for (const n of byID.values()) {
    const p = n.parentId ? byID.get(n.parentId) : undefined;
    (p ? p.children : out).push(n);
  }
  const sort = (a: Node[]) => { a.sort((x, y) => x.start - y.start); a.forEach((n) => sort(n.children)); };
  sort(out);
  return out;
});
const t0 = computed(() => (props.spans.length ? Math.min(...props.spans.map((s) => s.start)) : 0));
const total = computed(() => Math.max((props.spans.length ? Math.max(...props.spans.map((s) => s.start + s.duration)) : 1) - t0.value, 1e-6));
const rows = computed(() => {
  const out: { n: Node; depth: number; guides: boolean[]; last: boolean }[] = [];
  const walk = (nodes: Node[], depth: number, guides: boolean[]) => nodes.forEach((n, i) => {
    const last = i === nodes.length - 1;
    out.push({ n, depth, guides, last });
    if (n.children.length && !collapsed.value.has(n.spanId)) walk(n.children, depth + 1, [...guides, !last]);
  });
  walk(roots.value, 0, []);
  return out;
});
const grid = computed(() => `repeating-linear-gradient(to right, var(--border-1) 0 1px, transparent 1px ${100 / props.ticks}%)`);
const INDENT = 14;
const left = (n: Node) => ((n.start - t0.value) / total.value) * 100;
const width = (n: Node) => (n.duration / total.value) * 100;
function onKey(e: KeyboardEvent, n: Node) {
  const kids = n.children.length > 0, shut = collapsed.value.has(n.spanId);
  if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); emit('select', n.spanId, n); }
  if (kids && ((e.key === 'ArrowLeft' && !shut) || (e.key === 'ArrowRight' && shut))) { e.preventDefault(); toggle(n.spanId); }
}
</script>

<template>
  <div role="tree" class="ta-spantree">
    <div v-if="showRuler" class="ruler" :style="{ gridTemplateColumns: `${treeWidth}px minmax(0,1fr)` }">
      <div class="ruler-tree" />
      <div class="ruler-axis">
        <span v-for="i in ticks + 1" :key="i" :style="{ left: `${((i - 1) / ticks) * 100}%` }">{{ formatTime((total * (i - 1)) / ticks) }}</span>
      </div>
    </div>
    <div
      v-for="{ n, depth, guides, last } in rows"
      :key="n.spanId"
      role="treeitem"
      :aria-selected="n.spanId === selectedId"
      :aria-expanded="n.children.length ? !collapsed.has(n.spanId) : undefined"
      :aria-level="depth + 1"
      tabindex="0"
      class="ta-spantree__row"
      :class="{ 'is-selected': n.spanId === selectedId }"
      :style="{ gridTemplateColumns: `${treeWidth}px minmax(0,1fr)`, height: `${rowHeight}px` }"
      @click="emit('select', n.spanId, n)"
      @keydown="onKey($event, n)"
    >
      <div class="tree">
        <span v-for="(g, i) in guides" :key="i" class="guide" :class="{ on: g }" :style="{ width: `${INDENT}px` }" />
        <span v-if="depth > 0" class="elbow">
          <span :style="{ left: `${-INDENT + 6}px`, height: last ? '50%' : '100%' }" />
          <span :style="{ left: `${-INDENT + 6}px`, top: '50%', width: '6px', height: '1px' }" />
        </span>
        <span class="chevron" @click.stop="n.children.length && toggle(n.spanId)">
          <Icon v-if="n.children.length" name="chevron-right" :size="12" :style="{ transform: collapsed.has(n.spanId) ? 'none' : 'rotate(90deg)' }" />
        </span>
        <span class="swatch" :style="{ background: n.status === 'error' ? 'var(--state-error)' : color(n.service) }" />
        <span class="service">{{ n.service }}</span>
        <span class="name" :class="{ err: n.status === 'error' }">{{ n.name }}</span>
        <Icon v-if="n.status === 'error'" name="alert-triangle" :size="12" class="alert" />
      </div>
      <div class="lane" :style="{ backgroundImage: grid }">
        <div class="bar" :style="{ left: `${left(n)}%`, width: `max(${width(n)}%, 2px)`, background: n.status === 'error' ? 'var(--state-error)' : color(n.service) }">
          <span v-for="(ev, i) in n.events || []" :key="i" :title="ev.name" class="tick" :style="{ left: `${n.duration ? ((ev.time - n.start) / n.duration) * 100 : 0}%` }" />
        </div>
        <span class="ta-spantree__dur" :class="{ 'is-shown': depth === 0 || n.status === 'error' }" :style="{ left: `calc(${left(n) + width(n)}% + 6px)` }">{{ formatTime(n.duration) }}</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.ta-spantree {
  font: 400 12px/1 var(--font-mono);
  color: var(--fg-1);
  background: var(--surface-1);
  border: 1px solid var(--border-1);
  border-radius: var(--radius-2);
  overflow: hidden;
  min-width: 0;
}
.ruler {
  display: grid;
  height: 28px;
  border-bottom: 1px solid var(--border-1);
  background: var(--surface-2);
  color: var(--fg-4);
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
}
.ruler-tree {
  border-right: 1px solid var(--border-1);
}
.ruler-axis {
  position: relative;
  margin: 0 56px 0 8px;
}
.ruler-axis span {
  position: absolute;
  top: 0;
  bottom: 0;
  display: flex;
  align-items: center;
  padding-left: 4px;
  border-left: 1px solid var(--border-2);
  white-space: nowrap;
}
.ta-spantree__row {
  display: grid;
  cursor: pointer;
}
.ta-spantree__row.is-selected {
  background: var(--accent-subtle);
  box-shadow: inset 2px 0 0 var(--accent);
}
.tree {
  display: flex;
  align-items: center;
  min-width: 0;
  padding: 0 12px 0 6px;
  border-right: 1px solid var(--border-1);
}
.guide {
  flex: none;
  align-self: stretch;
}
.guide.on {
  background: linear-gradient(var(--border-2), var(--border-2)) 6px 0 / 1px 100% no-repeat;
}
.elbow {
  flex: none;
  width: 0;
  align-self: stretch;
  position: relative;
}
.elbow span {
  position: absolute;
  top: 0;
  width: 1px;
  background: var(--border-2);
}
.chevron {
  flex: none;
  width: 16px;
  height: 16px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--fg-3);
}
.chevron :deep(svg) {
  transition: transform var(--dur-fast) var(--ease-step);
}
.swatch {
  flex: none;
  width: 3px;
  height: 14px;
  margin-right: 8px;
  border-radius: 1px;
}
.service {
  flex: none;
  color: var(--fg-3);
  margin-right: 6px;
}
.name {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.name.err {
  color: var(--state-error);
}
.alert {
  margin-left: 6px;
  color: var(--state-error);
}
.lane {
  position: relative;
  margin: 0 56px 0 8px;
}
.bar {
  position: absolute;
  top: 50%;
  height: 10px;
  transform: translateY(-50%);
  border-radius: 1px;
  opacity: 0.85;
}
.is-selected .bar {
  opacity: 1;
}
.tick {
  position: absolute;
  top: -2px;
  bottom: -2px;
  width: 1px;
  background: var(--fg-1);
}
.ta-spantree__dur {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  white-space: nowrap;
  color: var(--fg-3);
  font-size: 11px;
}
</style>
