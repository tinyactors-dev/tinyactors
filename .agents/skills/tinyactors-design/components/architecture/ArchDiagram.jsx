import React from 'react';

const DIR = { left: [-1, 0], right: [1, 0], top: [0, -1], bottom: [0, 1] };
export const WIRE_KINDS = {
  call: { c: 'var(--fg-1)', w: 1.25, arrow: true, legend: 'sync call' },
  stream: { c: 'var(--fg-1)', w: 2.5, arrow: true, flow: true, legend: 'stream / queue' },
  shared: { c: 'var(--fg-2)', w: 1.5, dash: '1 3.5', pads: true, legend: 'shared memory · no copy' },
  copy: { c: 'var(--state-waiting)', w: 1.5, dash: '6 3', arrow: true, legend: 'copy' },
  dont: { c: 'var(--state-error)', w: 1.5, dash: '4 3', arrow: true, x: true, legend: "don't" },
};

function portPoint(b, p) {
  const at = p.at ?? 0.5;
  if (p.side === 'left') return [b.x, b.y + b.h * at];
  if (p.side === 'right') return [b.x + b.w, b.y + b.h * at];
  if (p.side === 'top') return [b.x + b.w * at, b.y];
  return [b.x + b.w * at, b.y + b.h];
}

function route(a, da, b, db, w) {
  const S = 14;
  const A1 = [a[0] + da[0] * S, a[1] + da[1] * S];
  const B1 = [b[0] + db[0] * S, b[1] + db[1] * S];
  const end = [b[0] + db[0] * 4, b[1] + db[1] * 4];
  if (w.via) return [a, A1, ...w.via, B1, end];
  const ha = da[0] !== 0, hb = db[0] !== 0;
  let mid;
  if (ha && hb) { const mx = w.mid ?? (A1[0] + B1[0]) / 2; mid = [[mx, A1[1]], [mx, B1[1]]]; }
  else if (ha && !hb) mid = [[B1[0], A1[1]]];
  else if (!ha && !hb) { const my = w.mid ?? (A1[1] + B1[1]) / 2; mid = [[A1[0], my], [B1[0], my]]; }
  else mid = [[A1[0], B1[1]]];
  return [a, A1, ...mid, B1, end];
}

function chamfer(pts, r = 7) {
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

const BLOCK = {
  system: { border: '1.5px solid var(--fg-1)', background: 'var(--surface-1)', boxShadow: 'inset 0 0 0 3px var(--surface-1), inset 0 0 0 3.5px var(--border-2)' },
  container: { border: '1.25px solid var(--fg-1)', background: 'var(--surface-1)' },
  component: { border: '1px solid var(--border-strong)', background: 'var(--surface-1)' },
  external: { border: '1.25px dashed var(--fg-3)', background: 'var(--bg-app)', color: 'var(--fg-2)' },
  memory: { border: '1.25px solid var(--fg-1)', background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)' },
  store: { border: '1.25px solid var(--fg-1)', borderBottom: '4px double var(--fg-1)', background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)' },
};
const label11 = { font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase' };

function ArchCanvas({ blocks = [], zones = [], wires = [], width, height, grid, tilt, onDive }) {
  const uid = 'ad' + React.useId().replace(/[^a-zA-Z0-9]/g, '');
  const byId = Object.fromEntries(blocks.map((b) => [b.id, b]));
  const end = (ref) => {
    const [bid, pid] = ref.split('.');
    const b = byId[bid]; if (!b) return null;
    const p = (b.ports || []).find((q) => q.id === pid) || { side: 'right' };
    return { pt: portPoint(b, p), dir: DIR[p.side] || DIR.right };
  };
  const drawn = wires.map((w, i) => {
    const A = end(w.from), B = end(w.to);
    if (!A || !B) return null;
    const k = WIRE_KINDS[w.kind || 'call'] || WIRE_KINDS.call;
    const color = w.hot ? 'var(--accent)' : k.c;
    return { i, w, k, color, A, B, ...chamfer(route(A.pt, A.dir, B.pt, B.dir, w)), mk: w.hot ? 'hot' : (w.kind || 'call') };
  }).filter(Boolean);
  const used = [...new Set(wires.map((w) => w.kind || 'call'))];
  const pads = [];
  blocks.forEach((b) => (b.ports || []).forEach((p) => pads.push({ key: b.id + '.' + p.id, pt: portPoint(b, p) })));
  const shared = new Set(drawn.filter((d) => d.k.pads).flatMap((d) => [d.w.from, d.w.to]));

  return (
      <div className="ta-arch__canvas" style={{ position: 'relative', width, height, background: grid ? 'var(--bg-grid), var(--surface-1)' : undefined }}>
        {zones.map((z, i) => (
          <div key={i} style={{ position: 'absolute', left: z.x, top: z.y, width: z.w, height: z.h, boxSizing: 'border-box', border: '1px dashed var(--border-strong)', background: z.hatch ? 'repeating-linear-gradient(135deg, var(--grid-minor) 0 1px, transparent 1px 7px)' : undefined }}>
            <span style={{ position: 'absolute', left: 8, top: 7, display: 'flex', alignItems: 'baseline', gap: 8, whiteSpace: 'nowrap' }}><span style={{ ...label11, color: 'var(--fg-3)' }}>{z.label}</span>{z.meta && <span style={{ font: '400 10px/1 var(--font-mono)', color: 'var(--fg-4)' }}>{z.meta}</span>}</span>
          </div>
        ))}
        <svg width={width} height={height} style={{ position: 'absolute', inset: 0, overflow: 'visible', pointerEvents: 'none' }}>
          <defs>
            {[...Object.keys(WIRE_KINDS), 'hot'].map((kk) => (
              <marker key={kk} id={`${uid}-${kk}`} viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="10" markerHeight="10" markerUnits="userSpaceOnUse" orient="auto-start-reverse">
                <path d="M1 1.5 L8.5 5 L1 8.5" fill="none" style={{ stroke: kk === 'hot' ? 'var(--accent)' : WIRE_KINDS[kk].c, strokeWidth: 1.4 }} />
              </marker>
            ))}
          </defs>
          {drawn.map(({ i, w, k, color, d, mk }) => (
            <g key={i}>
              <path d={d} fill="none" strokeLinecap={k.dash === '1 3.5' ? 'round' : 'butt'} strokeLinejoin="round"
                markerEnd={k.arrow ? `url(#${uid}-${mk})` : undefined} markerStart={k.arrow && w.both ? `url(#${uid}-${mk})` : undefined}
                style={{ stroke: color, strokeWidth: w.hot && k.w < 2 ? 1.75 : k.w, strokeDasharray: k.dash }} />
              {k.flow && w.animate !== false && <path className="ta-arch__flow" d={d} fill="none" style={{ stroke: 'var(--surface-1)', strokeWidth: 1, strokeDasharray: '2 8' }} />}
            </g>
          ))}
        </svg>
        {tilt && blocks.map((b) => (
          <React.Fragment key={'side' + b.id}>
            <span className="ta-arch__side ta-arch__side--s" style={{ left: b.x, top: b.y + b.h, width: b.w, height: 14 }} />
            <span className="ta-arch__side ta-arch__side--e" style={{ left: b.x + b.w, top: b.y, width: 14, height: b.h }} />
            <span className="ta-arch__side ta-arch__side--w" style={{ left: b.x - 14, top: b.y, width: 14, height: b.h }} />
          </React.Fragment>
        ))}
        {blocks.map((b) => {
          const kind = BLOCK[b.kind] ? b.kind : 'container';
          const emph = b.emphasis === 'hot' ? 'var(--accent)' : b.emphasis === 'warn' ? 'var(--state-error)' : null;
          return (
            <div key={b.id} title={b.note} className={'ta-arch__block' + (b.inner && onDive ? ' ta-arch__block--dive' : '')} onClick={b.inner && onDive ? () => onDive(b) : undefined} style={{ position: 'absolute', left: b.x, top: b.y, width: b.w, height: b.h, boxSizing: 'border-box', padding: '9px 12px', display: 'flex', flexDirection: 'column', gap: 3, overflow: 'hidden', color: 'var(--fg-1)', ...BLOCK[kind], ...(emph ? { borderColor: emph } : null) }}>
              {b.label && <span style={{ ...label11, fontSize: 10, color: emph || 'var(--fg-4)', whiteSpace: 'nowrap' }}>{b.label}</span>}
              <span style={{ font: '500 13px/1.15 var(--font-mono)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{b.name}</span>
              {b.tech && b.h >= 64 && <span style={{ font: '400 11px/1.2 var(--font-mono)', color: 'var(--fg-3)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{b.tech}</span>}
              {b.note && b.h >= 96 && <span style={{ font: '400 12px/1.35 var(--font-sans)', color: 'var(--fg-2)', marginTop: 2, textWrap: 'pretty' }}>{b.note}</span>}
              {(b.ports || []).filter((p) => p.label).map((p) => {
                const at = (p.at ?? 0.5) * 100 + '%';
                const pos = p.side === 'left' ? { left: 7, top: at, transform: 'translateY(-50%)' } : p.side === 'right' ? { right: 7, top: at, transform: 'translateY(-50%)' } : p.side === 'top' ? { top: 3, left: at, transform: 'translateX(-50%)' } : { bottom: 3, left: at, transform: 'translateX(-50%)' };
                return <span key={p.id} style={{ position: 'absolute', ...pos, font: '400 10px/1 var(--font-mono)', color: 'var(--fg-3)', whiteSpace: 'nowrap', background: 'inherit' }}>{p.label}</span>;
              })}
            </div>
          );
        })}
        <svg width={width} height={height} style={{ position: 'absolute', inset: 0, overflow: 'visible', pointerEvents: 'none' }}>
          {pads.map(({ key, pt }) => <circle key={key} cx={pt[0]} cy={pt[1]} r="3.5" style={{ fill: shared.has(key) ? 'var(--fg-1)' : 'var(--surface-1)', stroke: 'var(--fg-1)', strokeWidth: 1.25 }} />)}
          {drawn.map(({ i, w, k, color, lp, horiz }) => {
            const [dx, dy] = w.labelOffset || [0, 0];
            const tx = lp[0] + (horiz ? 0 : 8) + dx, ty = lp[1] + (horiz ? -8 : 4) + dy;
            return (
              <g key={i}>
                {k.x && <g transform={`translate(${lp[0]} ${lp[1]})`}><circle r="7" style={{ fill: 'var(--surface-1)', stroke: color, strokeWidth: 1.25 }} /><path d="M-3 -3 L3 3 M3 -3 L-3 3" style={{ stroke: color, strokeWidth: 1.4 }} /></g>}
                {w.label && <text x={k.x && horiz ? tx : k.x ? tx + 4 : tx} y={k.x && horiz ? ty - 4 : ty} textAnchor={horiz ? 'middle' : 'start'} style={{ font: '400 11px var(--font-mono)', fill: w.hot ? 'var(--accent-fg)' : k.x ? 'var(--state-error)' : 'var(--fg-2)', paintOrder: 'stroke', stroke: 'var(--surface-1)', strokeWidth: 4, strokeLinejoin: 'round' }}>{w.label}</text>}
              </g>
            );
          })}
        </svg>
      </div>
  );
}

export function ArchDiagram({ blocks = [], zones = [], wires = [], width = 720, height = 420, grid = true, legend = false, tilt = false, rootLabel = 'system', style, ...rest }) {
  const [path, setPath] = React.useState([]);
  const [depth, setDepth] = React.useState(0);
  const timer = React.useRef(0);
  React.useEffect(() => () => clearTimeout(timer.current), []);
  const dive = (b) => { clearTimeout(timer.current); setPath((p) => [...p.slice(0, depth), b]); requestAnimationFrame(() => requestAnimationFrame(() => setDepth((d) => d + 1))); };
  const up = (to) => { setDepth(to); clearTimeout(timer.current); timer.current = setTimeout(() => setPath((p) => p.slice(0, to)), 900); };
  const levels = [{ blocks, zones, wires }, ...path.map((b) => b.inner)];
  const cur = levels[Math.min(depth, levels.length - 1)] || levels[0];
  const used = [...new Set((cur.wires || []).map((w) => w.kind || 'call'))];
  const origin = (b) => b ? `${b.x + b.w / 2}px ${b.y + b.h / 2}px` : '50% 50%';
  return (
    <div className={'ta-arch' + (tilt ? ' ta-arch--tilt' : '')} style={{ minWidth: 0, overflowX: tilt ? 'visible' : 'auto', overflowY: tilt ? 'visible' : 'hidden', ...style }} {...rest}>
      <div style={{ position: 'relative', width, height, overflow: path.length ? 'hidden' : 'visible' }}>
        {path.length > 0 && (
          <nav className="ta-arch__crumb" aria-label="Diagram level">
            {[rootLabel, ...path.map((b) => b.name)].slice(0, depth + 1).map((n, i, a) => i < a.length - 1
              ? <React.Fragment key={i}><button type="button" onClick={() => up(i)}>{n}</button><span>/</span></React.Fragment>
              : <b key={i}>{n}</b>)}
          </nav>
        )}
        {levels.map((l, i) => (
          <div key={i} className={'ta-arch__layer' + (i < depth ? ' is-away' : i > depth ? ' is-gone' : '')} aria-hidden={i !== depth} style={{ transformOrigin: origin(i < depth ? path[i] : path[i - 1]) }}>
            <ArchCanvas blocks={l.blocks || []} zones={l.zones || []} wires={l.wires || []} width={l.width || width} height={l.height || height} grid={grid} tilt={tilt} onDive={i === depth ? dive : undefined} />
          </div>
        ))}
      </div>
      {legend && used.length > 0 && (
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '6px 18px', padding: '10px 2px 0', font: '400 11px/1 var(--font-mono)', color: 'var(--fg-2)' }}>
          {used.map((kk) => { const k = WIRE_KINDS[kk] || WIRE_KINDS.call; return (
            <span key={kk} style={{ display: 'inline-flex', alignItems: 'center', gap: 7, whiteSpace: 'nowrap' }}>
              <svg width="26" height="10"><line x1="1" y1="5" x2="25" y2="5" style={{ stroke: k.c, strokeWidth: k.w, strokeDasharray: k.dash, strokeLinecap: k.pads ? 'round' : 'butt' }} /></svg>{k.legend}
            </span>
          ); })}
        </div>
      )}
    </div>
  );
}
