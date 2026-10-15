import React from 'react';

const HATCH = 'repeating-linear-gradient(135deg, var(--border-2) 0 1px, transparent 1px 5px), var(--surface-1)';
export const FIELD_KINDS = {
  data: { bg: 'var(--surface-1)', fg: 'var(--fg-1)', legend: 'data' },
  hot: { bg: 'var(--state-running-bg)', fg: 'var(--state-running)', legend: 'hot path' },
  warm: { bg: 'var(--state-waiting-bg)', fg: 'var(--state-waiting)', legend: 'warm' },
  cold: { bg: 'var(--surface-2)', fg: 'var(--fg-2)', legend: 'cold' },
  ptr: { bg: 'color-mix(in srgb, var(--data-4) 26%, var(--surface-1))', fg: 'var(--fg-1)', legend: 'pointer' },
  atomic: { bg: 'var(--state-done-bg)', fg: 'var(--state-done)', legend: 'atomic · cross-core' },
  pad: { bg: HATCH, fg: 'var(--fg-4)', legend: 'padding' },
};

const natural = (s) => ([1, 2, 4, 8].includes(s) ? s : s % 8 === 0 ? 8 : 1);

export function layoutStruct(fields, { packed = false } = {}) {
  let off = 0, maxA = 1;
  const out = [];
  for (const f of fields) {
    const a = packed ? 1 : f.align ?? natural(f.size);
    maxA = Math.max(maxA, a);
    const at = f.offset ?? Math.ceil(off / a) * a;
    if (at > off) out.push({ kind: 'pad', size: at - off, offset: off, name: '' });
    out.push({ kind: 'data', ...f, offset: at });
    off = at + f.size;
  }
  const size = packed ? off : Math.ceil(off / maxA) * maxA;
  if (size > off) out.push({ kind: 'pad', size: size - off, offset: off, name: '', tail: true });
  return { fields: out, size };
}

const lbl = { font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase' };

export function MemoryLayout({ name, fields = [], packed = false, lineSize = 64, bytesPerRow, rowHeight = 44, table = false, summary = true, legend = true, style, ...rest }) {
  const bpr = bytesPerRow || Math.min(lineSize, 64);
  const { fields: laid, size } = layoutStruct(fields, { packed });
  const rows = Math.max(1, Math.ceil(size / bpr));
  const straddles = (f) => f.kind !== 'pad' && f.size <= lineSize && Math.floor(f.offset / lineSize) !== Math.floor((f.offset + f.size - 1) / lineSize);
  const segs = [];
  laid.forEach((f, fi) => {
    let s = f.offset; const e = f.offset + f.size;
    while (s < e) { const r = Math.floor(s / bpr), se = Math.min(e, (r + 1) * bpr); segs.push({ f, fi, r, col: s % bpr, span: se - s, first: s === f.offset, last: se === e }); s = se; }
  });
  const padBytes = laid.filter((f) => f.kind === 'pad').reduce((a, f) => a + f.size, 0);
  const lines = Math.ceil(size / lineSize);
  const bad = laid.filter(straddles);
  const kinds = [...new Set(laid.map((f) => f.kind))].filter((k) => FIELD_KINDS[k] && k !== 'data' && k !== 'pad');
  const tick = bpr >= 32 ? 8 : 4;

  return (
    <div className="ta-mem" style={{ minWidth: 0, color: 'var(--fg-1)', ...style }} {...rest}>
      {name && <div style={{ display: 'flex', alignItems: 'baseline', gap: 10, marginBottom: 8 }}><span style={{ font: '500 13px var(--font-mono)', whiteSpace: 'pre', flex: 'none' }}>struct {name}</span></div>}
      <div style={{ display: 'grid', gridTemplateColumns: '44px minmax(0,1fr)', rowGap: 6 }}>
        <span />
        <div style={{ position: 'relative', height: 12 }}>
          {Array.from({ length: bpr / tick }, (_, i) => <span key={i} style={{ position: 'absolute', left: `${((i * tick) / bpr) * 100}%`, bottom: 0, font: '400 10px/1 var(--font-mono)', color: 'var(--fg-4)', paddingLeft: 2, borderLeft: '1px solid var(--border-2)', height: 12, display: 'flex', alignItems: 'flex-end' }}>{i * tick}</span>)}
        </div>
        {Array.from({ length: rows }, (_, r) => {
          const lineStart = (r * bpr) % lineSize === 0;
          return (
            <React.Fragment key={r}>
              <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center', gap: 3, font: '400 10px/1 var(--font-mono)', color: 'var(--fg-3)' }}>
                <span>+{r * bpr}</span>
              </div>
              <div style={{ display: 'grid', gridTemplateColumns: `repeat(${bpr}, minmax(0,1fr))`, height: rowHeight, border: '1.25px solid var(--fg-1)', background: `repeating-linear-gradient(to right, transparent 0 calc(100% / ${bpr} - 1px), var(--border-1) calc(100% / ${bpr} - 1px) calc(100% / ${bpr}))`, marginTop: lineStart && r > 0 && bpr < lineSize ? 6 : 0 }}>
                {segs.filter((s) => s.r === r).map((s, i) => {
                  const k = FIELD_KINDS[s.f.kind] || FIELD_KINDS.data;
                  const warn = s.f.warn || straddles(s.f);
                  const isPad = s.f.kind === 'pad';
                  return (
                    <div key={i} title={isPad ? `${s.f.size} B padding @ +${s.f.offset}` : `${s.f.type ? s.f.type + ' ' : ''}${s.f.name} · ${s.f.size} B @ +${s.f.offset}${s.f.note ? ' — ' + s.f.note : ''}`}
                      style={{ gridColumn: `${s.col + 1} / span ${s.span}`, minWidth: 0, background: k.bg, boxShadow: `${s.last ? 'inset -1px 0 0 var(--fg-1)' : 'inset 0 0 0 0 transparent'}${warn ? ', inset 0 0 0 1.5px var(--state-error)' : ''}`, padding: '5px 6px', display: 'flex', flexDirection: 'column', justifyContent: 'center', gap: 3, overflow: 'hidden' }}>
                      {!isPad && <span style={{ font: '500 11px/1 var(--font-mono)', color: warn ? 'var(--state-error)' : s.first ? k.fg : 'var(--fg-3)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{s.f.name}</span>}
                    </div>
                  );
                })}
              </div>
            </React.Fragment>
          );
        })}
      </div>
      {summary && (
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '4px 14px', marginTop: 10, paddingLeft: 44, font: '400 11px/1.3 var(--font-mono)', color: 'var(--fg-2)' }}>
          <span><span style={{ color: 'var(--fg-1)', fontWeight: 500 }}>sizeof = {size} B</span></span>
          {bad.length > 0 && <span style={{ color: 'var(--state-error)' }}>{bad.map((f) => f.name).join(', ')} straddle{bad.length === 1 ? 's' : ''} a line boundary</span>}
        </div>
      )}
      {legend && kinds.length > 0 && (
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '6px 14px', marginTop: 8, paddingLeft: 44, font: '400 11px/1 var(--font-mono)', color: 'var(--fg-3)' }}>
          {kinds.map((kk) => <span key={kk} style={{ display: 'inline-flex', alignItems: 'center', gap: 6, whiteSpace: 'nowrap' }}><i style={{ width: 12, height: 12, background: FIELD_KINDS[kk].bg, border: '1px solid var(--fg-1)' }} />{FIELD_KINDS[kk].legend}</span>)}
        </div>
      )}
      {table && (
        <div style={{ marginTop: 12, marginLeft: 44, border: '1px solid var(--border-1)', font: '400 12px/1 var(--font-mono)' }}>
          {laid.map((f, i) => (
            <div key={i} style={{ display: 'grid', gridTemplateColumns: '56px 48px minmax(0,1.2fr) minmax(0,1fr) minmax(0,1.4fr)', gap: 10, alignItems: 'center', height: 26, padding: '0 10px', borderTop: i ? '1px solid var(--border-1)' : 0, color: f.kind === 'pad' ? 'var(--fg-4)' : 'var(--fg-1)', background: f.kind === 'pad' ? HATCH : undefined }}>
              <span style={{ color: 'var(--fg-3)' }}>+{f.offset}</span><span style={{ color: 'var(--fg-3)' }}>{f.size} B</span>
              {f.kind === 'pad' ? <span style={{ gridColumn: '3 / span 3' }} /> : <>
                <span style={{ color: 'var(--fg-2)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.type}</span>
                <span style={{ fontWeight: 500, color: straddles(f) || f.warn ? 'var(--state-error)' : undefined, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.name}</span>
                <span style={{ font: '400 12px/1.2 var(--font-sans)', color: 'var(--fg-3)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.note}</span>
              </>}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
