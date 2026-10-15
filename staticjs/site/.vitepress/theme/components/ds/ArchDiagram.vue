<script setup lang="ts">
// A PCB-style architecture drawing: blocks, zones, and wires whose kind says
// what a hop costs. A port of the Tinyactors design system's ArchDiagram
// (tinyactors-dev/website, components/architecture/ArchDiagram.jsx),
// without its drill-down and tilt modes. Labels keep the system's sizes but
// not its uppercase, which this site leaves out.
import { computed, useId } from 'vue';
import { type Block, type Wire, type Zone, WIRE_KINDS, drawWires, portPoint } from './arch';

const props = withDefaults(
  defineProps<{ blocks: Block[]; zones?: Zone[]; wires?: Wire[]; width?: number; height?: number; grid?: boolean; legend?: boolean }>(),
  { zones: () => [], wires: () => [], width: 720, height: 420, grid: true, legend: false },
);

const uid = 'ad' + useId().replace(/[^a-zA-Z0-9]/g, '');
const drawn = computed(() => drawWires(props.blocks, props.wires));
const pads = computed(() => props.blocks.flatMap((b) => (b.ports || []).map((p) => ({ key: `${b.id}.${p.id}`, pt: portPoint(b, p) }))));
const shared = computed(() => new Set(drawn.value.filter((d) => 'pads' in d.k).flatMap((d) => [d.w.from, d.w.to])));
const used = computed(() => [...new Set(props.wires.map((w) => w.kind || 'call'))]);
const markers = [...Object.keys(WIRE_KINDS), 'hot'];

const BLOCK: Record<string, Record<string, string>> = {
  system: { border: '1.5px solid var(--fg-1)', background: 'var(--surface-1)', boxShadow: 'inset 0 0 0 3px var(--surface-1), inset 0 0 0 3.5px var(--border-2)' },
  container: { border: '1.25px solid var(--fg-1)', background: 'var(--surface-1)' },
  component: { border: '1px solid var(--border-strong)', background: 'var(--surface-1)' },
  external: { border: '1.25px dashed var(--fg-3)', background: 'var(--bg-app)', color: 'var(--fg-2)' },
  memory: { border: '1.25px solid var(--fg-1)', background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)' },
  store: { border: '1.25px solid var(--fg-1)', borderBottom: '4px double var(--fg-1)', background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)' },
};

function blockStyle(b: Block) {
  const emph = b.emphasis === 'hot' ? 'var(--accent)' : b.emphasis === 'warn' ? 'var(--state-error)' : null;
  return { left: `${b.x}px`, top: `${b.y}px`, width: `${b.w}px`, height: `${b.h}px`, ...BLOCK[b.kind && BLOCK[b.kind] ? b.kind : 'container'], ...(emph ? { borderColor: emph } : {}) };
}
function portStyle(p: { side: string; at?: number }) {
  const at = `${(p.at ?? 0.5) * 100}%`;
  if (p.side === 'left') return { left: '7px', top: at, transform: 'translateY(-50%)' };
  if (p.side === 'right') return { right: '7px', top: at, transform: 'translateY(-50%)' };
  if (p.side === 'top') return { top: '3px', left: at, transform: 'translateX(-50%)' };
  return { bottom: '3px', left: at, transform: 'translateX(-50%)' };
}
function labelAt(d: ReturnType<typeof drawWires>[number]) {
  const [dx, dy] = d.w.labelOffset || [0, 0];
  const tx = d.lp[0] + (d.horiz ? 0 : 8) + dx, ty = d.lp[1] + (d.horiz ? -8 : 4) + dy;
  const x = 'x' in d.k;
  return { x: x && d.horiz ? tx : x ? tx + 4 : tx, y: x && d.horiz ? ty - 4 : ty, anchor: d.horiz ? 'middle' : 'start' };
}
</script>

<template>
  <div class="ta-arch" :style="{ minWidth: 0, overflowX: 'auto', overflowY: 'hidden' }">
    <div class="ta-arch__canvas" :style="{ position: 'relative', width: `${width}px`, height: `${height}px`, background: grid ? 'var(--bg-grid), var(--surface-1)' : undefined }">
      <div
        v-for="(z, i) in zones"
        :key="i"
        class="ta-arch__zone"
        :style="{ left: `${z.x}px`, top: `${z.y}px`, width: `${z.w}px`, height: `${z.h}px`, background: z.hatch ? 'repeating-linear-gradient(135deg, var(--grid-minor) 0 1px, transparent 1px 7px)' : undefined }"
      >
        <span class="ta-arch__zone-label"><span>{{ z.label }}</span><span v-if="z.meta" class="meta">{{ z.meta }}</span></span>
      </div>
      <svg :width="width" :height="height" class="ta-arch__svg">
        <defs>
          <marker v-for="kk in markers" :id="`${uid}-${kk}`" :key="kk" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="10" markerHeight="10" markerUnits="userSpaceOnUse" orient="auto-start-reverse">
            <path d="M1 1.5 L8.5 5 L1 8.5" fill="none" :style="{ stroke: kk === 'hot' ? 'var(--accent)' : (WIRE_KINDS as any)[kk].c, strokeWidth: 1.4 }" />
          </marker>
        </defs>
        <g v-for="d in drawn" :key="d.i">
          <path
            :d="d.d"
            fill="none"
            :stroke-linecap="'dash' in d.k && d.k.dash === '1 3.5' ? 'round' : 'butt'"
            stroke-linejoin="round"
            :marker-end="'arrow' in d.k ? `url(#${uid}-${d.mk})` : undefined"
            :marker-start="'arrow' in d.k && d.w.both ? `url(#${uid}-${d.mk})` : undefined"
            :style="{ stroke: d.color, strokeWidth: d.w.hot && d.k.w < 2 ? 1.75 : d.k.w, strokeDasharray: 'dash' in d.k ? d.k.dash : undefined }"
          />
          <path v-if="'flow' in d.k && d.w.animate !== false" class="ta-arch__flow" :d="d.d" fill="none" :style="{ stroke: 'var(--surface-1)', strokeWidth: 1, strokeDasharray: '2 8' }" />
        </g>
      </svg>
      <div v-for="b in blocks" :key="b.id" :title="b.note" class="ta-arch__block" :style="blockStyle(b)">
        <span v-if="b.label" class="ta-arch__label" :style="{ color: b.emphasis === 'hot' ? 'var(--accent)' : 'var(--fg-4)' }">{{ b.label }}</span>
        <span class="ta-arch__name">{{ b.name }}</span>
        <span v-if="b.tech && b.h >= 64" class="ta-arch__tech">{{ b.tech }}</span>
        <span v-if="b.note && b.h >= 96" class="ta-arch__note">{{ b.note }}</span>
        <span v-for="p in (b.ports || []).filter((q) => q.label)" :key="p.id" class="ta-arch__port" :style="portStyle(p)">{{ p.label }}</span>
      </div>
      <svg :width="width" :height="height" class="ta-arch__svg">
        <circle v-for="p in pads" :key="p.key" :cx="p.pt[0]" :cy="p.pt[1]" r="3.5" :style="{ fill: shared.has(p.key) ? 'var(--fg-1)' : 'var(--surface-1)', stroke: 'var(--fg-1)', strokeWidth: 1.25 }" />
        <g v-for="d in drawn" :key="d.i">
          <g v-if="'x' in d.k" :transform="`translate(${d.lp[0]} ${d.lp[1]})`">
            <circle r="7" :style="{ fill: 'var(--surface-1)', stroke: d.color, strokeWidth: 1.25 }" />
            <path d="M-3 -3 L3 3 M3 -3 L-3 3" :style="{ stroke: d.color, strokeWidth: 1.4 }" />
          </g>
          <text
            v-if="d.w.label"
            :x="labelAt(d).x"
            :y="labelAt(d).y"
            :text-anchor="labelAt(d).anchor"
            class="ta-arch__wire-label"
            :style="{ fill: d.w.hot ? 'var(--accent-fg)' : 'x' in d.k ? 'var(--state-error)' : 'var(--fg-2)' }"
          >{{ d.w.label }}</text>
        </g>
      </svg>
    </div>
    <div v-if="legend && used.length" class="ta-arch__legend">
      <span v-for="kk in used" :key="kk">
        <svg width="26" height="10"><line x1="1" y1="5" x2="25" y2="5" :style="{ stroke: WIRE_KINDS[kk].c, strokeWidth: WIRE_KINDS[kk].w, strokeDasharray: 'dash' in WIRE_KINDS[kk] ? (WIRE_KINDS[kk] as any).dash : undefined, strokeLinecap: 'pads' in WIRE_KINDS[kk] ? 'round' : 'butt' }" /></svg>
        {{ WIRE_KINDS[kk].legend }}
      </span>
    </div>
  </div>
</template>

<style scoped>
.ta-arch__zone {
  position: absolute;
  box-sizing: border-box;
  border: 1px dashed var(--border-strong);
}
.ta-arch__zone-label {
  position: absolute;
  left: 8px;
  top: 7px;
  display: flex;
  align-items: baseline;
  gap: 8px;
  white-space: nowrap;
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  color: var(--fg-3);
}
.ta-arch__zone-label .meta {
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-4);
}
.ta-arch__svg {
  position: absolute;
  inset: 0;
  overflow: visible;
  pointer-events: none;
}
.ta-arch__block {
  position: absolute;
  box-sizing: border-box;
  padding: 9px 12px;
  display: flex;
  flex-direction: column;
  gap: 3px;
  overflow: hidden;
  color: var(--fg-1);
}
.ta-arch__label {
  font: var(--type-label);
  font-size: 10px;
  letter-spacing: var(--tracking-label);
  white-space: nowrap;
}
.ta-arch__name {
  font: 500 13px/1.15 var(--font-mono);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.ta-arch__tech {
  font: 400 11px/1.2 var(--font-mono);
  color: var(--fg-3);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.ta-arch__note {
  font: 400 12px/1.35 var(--font-sans);
  color: var(--fg-2);
  margin-top: 2px;
  text-wrap: pretty;
}
.ta-arch__port {
  position: absolute;
  font: 400 10px/1 var(--font-mono);
  color: var(--fg-3);
  white-space: nowrap;
  background: inherit;
}
.ta-arch__wire-label {
  font: 400 11px var(--font-mono);
  paint-order: stroke;
  stroke: var(--surface-1);
  stroke-width: 4px;
  stroke-linejoin: round;
}
.ta-arch__legend {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 18px;
  padding: 10px 2px 0;
  font: 400 11px/1 var(--font-mono);
  color: var(--fg-2);
}
.ta-arch__legend span {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  white-space: nowrap;
}
</style>
