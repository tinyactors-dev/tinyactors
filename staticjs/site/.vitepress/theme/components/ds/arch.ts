// Geometry of the Tinyactors design system's ArchDiagram (tinyactors-dev/
// website, components/architecture/ArchDiagram.jsx): ports, orthogonal
// routes with 45° chamfers, and the kinds of wire.

export type Side = 'left' | 'right' | 'top' | 'bottom';
export interface Port { id: string; side: Side; at?: number; label?: string }
export interface Block {
  id: string;
  kind?: 'system' | 'container' | 'component' | 'external' | 'memory' | 'store';
  name: string;
  label?: string;
  tech?: string;
  note?: string;
  emphasis?: 'hot' | 'warn';
  x: number; y: number; w: number; h: number;
  ports?: Port[];
}
export interface Zone { x: number; y: number; w: number; h: number; label: string; meta?: string; hatch?: boolean }
export interface Wire {
  from: string; to: string;
  kind?: keyof typeof WIRE_KINDS;
  label?: string; hot?: boolean; both?: boolean; animate?: boolean;
  mid?: number; via?: [number, number][]; labelOffset?: [number, number];
}

export const WIRE_KINDS = {
  call: { c: 'var(--fg-1)', w: 1.25, arrow: true, legend: 'sync call' },
  stream: { c: 'var(--fg-1)', w: 2.5, arrow: true, flow: true, legend: 'stream / queue' },
  shared: { c: 'var(--fg-2)', w: 1.5, dash: '1 3.5', pads: true, legend: 'shared memory · no copy' },
  copy: { c: 'var(--state-waiting)', w: 1.5, dash: '6 3', arrow: true, legend: 'copy' },
  dont: { c: 'var(--state-error)', w: 1.5, dash: '4 3', arrow: true, x: true, legend: "don't" },
} as const satisfies Record<string, { c: string; w: number; arrow?: boolean; flow?: boolean; dash?: string; pads?: boolean; x?: boolean; legend: string }>;

const DIR: Record<Side, [number, number]> = { left: [-1, 0], right: [1, 0], top: [0, -1], bottom: [0, 1] };

export function portPoint(b: Block, p: Port): [number, number] {
  const at = p.at ?? 0.5;
  if (p.side === 'left') return [b.x, b.y + b.h * at];
  if (p.side === 'right') return [b.x + b.w, b.y + b.h * at];
  if (p.side === 'top') return [b.x + b.w * at, b.y];
  return [b.x + b.w * at, b.y + b.h];
}

function route(a: number[], da: number[], b: number[], db: number[], w: Wire): number[][] {
  const S = 14;
  const A1 = [a[0] + da[0] * S, a[1] + da[1] * S];
  const B1 = [b[0] + db[0] * S, b[1] + db[1] * S];
  const end = [b[0] + db[0] * 4, b[1] + db[1] * 4];
  if (w.via) return [a, A1, ...w.via, B1, end];
  const ha = da[0] !== 0, hb = db[0] !== 0;
  let mid: number[][];
  if (ha && hb) { const mx = w.mid ?? (A1[0] + B1[0]) / 2; mid = [[mx, A1[1]], [mx, B1[1]]]; }
  else if (ha && !hb) mid = [[B1[0], A1[1]]];
  else if (!ha && !hb) { const my = w.mid ?? (A1[1] + B1[1]) / 2; mid = [[A1[0], my], [B1[0], my]]; }
  else mid = [[A1[0], B1[1]]];
  return [a, A1, ...mid, B1, end];
}

function chamfer(pts: number[][], r = 7) {
  const p = pts.filter((q, i) => i === 0 || q[0] !== pts[i - 1][0] || q[1] !== pts[i - 1][1]);
  const s = [p[0]];
  for (let i = 1; i < p.length - 1; i++) {
    const a = s[s.length - 1], b = p[i], c = p[i + 1];
    if (Math.abs((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])) > 1e-6) s.push(b);
  }
  s.push(p[p.length - 1]);
  let d = `M${s[0][0]} ${s[0][1]}`;
  for (let i = 1; i < s.length - 1; i++) {
    const a = s[i - 1], b = s[i], c = s[i + 1];
    const l1 = Math.hypot(b[0] - a[0], b[1] - a[1]), l2 = Math.hypot(c[0] - b[0], c[1] - b[1]);
    const k = Math.min(r, l1 / 2, l2 / 2);
    d += ` L${b[0] - ((b[0] - a[0]) / l1) * k} ${b[1] - ((b[1] - a[1]) / l1) * k} L${b[0] + ((c[0] - b[0]) / l2) * k} ${b[1] + ((c[1] - b[1]) / l2) * k}`;
  }
  const e = s[s.length - 1];
  d += ` L${e[0]} ${e[1]}`;
  let best = 0, lp = [(s[0][0] + e[0]) / 2, (s[0][1] + e[1]) / 2], horiz = true;
  for (let i = 1; i < s.length; i++) {
    const l = Math.hypot(s[i][0] - s[i - 1][0], s[i][1] - s[i - 1][1]);
    if (l > best) { best = l; lp = [(s[i][0] + s[i - 1][0]) / 2, (s[i][1] + s[i - 1][1]) / 2]; horiz = s[i][1] === s[i - 1][1]; }
  }
  return { d, lp, horiz };
}

/** Each wire's path, label point and style, from the blocks it joins. */
export function drawWires(blocks: Block[], wires: Wire[]) {
  const byID = Object.fromEntries(blocks.map((b) => [b.id, b]));
  const end = (ref: string) => {
    const [bid, pid] = ref.split('.');
    const b = byID[bid];
    if (!b) return null;
    const p = (b.ports || []).find((q) => q.id === pid) || { id: pid, side: 'right' as Side };
    return { pt: portPoint(b, p), dir: DIR[p.side] || DIR.right };
  };
  return wires.flatMap((w, i) => {
    const A = end(w.from), B = end(w.to);
    if (!A || !B) return [];
    const k = WIRE_KINDS[w.kind || 'call'] || WIRE_KINDS.call;
    const color = w.hot ? 'var(--accent)' : k.c;
    return [{ i, w, k, color, ...chamfer(route(A.pt, A.dir, B.pt, B.dir, w)), mk: w.hot ? 'hot' : (w.kind || 'call') }];
  });
}
