import React from 'react';

const lbl = { font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase' };

export function RingBuffer({ name, capacity = 8, head = 0, tail = 0, full = false, items = [], slotSize, producer, consumer, mode, style, ...rest }) {
  const n = full ? capacity : (tail - head + capacity) % capacity;
  const slots = Array.from({ length: capacity }, (_, i) => {
    const k = (i - head + capacity) % capacity;
    return { i, used: k < n, item: k < n ? items[k] : undefined };
  });
  const ptr = (label, who, color) => (
    <span style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 3, font: '500 11px/1 var(--font-mono)', color, whiteSpace: 'nowrap' }}>
      <span style={{ width: 0, height: 0, borderLeft: '5px solid transparent', borderRight: '5px solid transparent', borderBottom: `6px solid ${color}` }} />
      {label}{who && <span style={{ font: '400 10px/1 var(--font-mono)', color: 'var(--fg-3)' }}>{who}</span>}
    </span>
  );
  return (
    <div className="ta-ring" style={{ minWidth: 0, color: 'var(--fg-1)', ...style }} {...rest}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginBottom: 8, flexWrap: 'wrap' }}>
        {name && <span style={{ font: '500 13px var(--font-mono)' }}>{name}</span>}
        {mode && <span style={{ ...lbl, color: 'var(--fg-4)' }}>{mode}</span>}
        {slotSize && <span style={{ font: '400 11px var(--font-mono)', color: 'var(--fg-3)', marginLeft: 'auto' }}>{slotSize}/slot</span>}
      </div>
      <div style={{ position: 'relative', display: 'grid', gridTemplateColumns: `repeat(${capacity}, minmax(0,1fr))`, columnGap: 4, rowGap: 6, paddingBottom: 22 }}>
        {slots.map((s) => <span key={'i' + s.i} style={{ gridRow: 1, gridColumn: s.i + 1, font: '400 10px/1 var(--font-mono)', color: 'var(--fg-4)', textAlign: 'center' }}>{s.i === 0 || s.i === capacity - 1 ? s.i : ''}</span>)}
        {slots.map((s) => {
          const next = !full && s.i === tail;
          return (
            <div key={'s' + s.i} title={s.used ? String(s.item ?? '') : next ? 'next write' : 'free'} style={{ gridRow: 2, gridColumn: s.i + 1, height: 44, minWidth: 0, boxSizing: 'border-box', border: next ? '1.25px dashed var(--accent)' : '1.25px solid var(--fg-1)', background: s.used ? 'var(--state-waiting-bg)' : 'var(--surface-1)', display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '0 4px', overflow: 'hidden' }}>
              <span style={{ font: '500 11px/1 var(--font-mono)', color: s.used ? 'var(--fg-1)' : next ? 'var(--accent-fg)' : 'var(--fg-4)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{s.used ? s.item ?? '●' : ''}</span>
            </div>
          );
        })}
        {head === tail
          ? <div style={{ gridRow: 3, gridColumn: head + 1, justifySelf: 'center' }}>{ptr('head · tail', null, full ? 'var(--fg-1)' : 'var(--accent-fg)')}</div>
          : <>
              <div style={{ gridRow: 3, gridColumn: head + 1, justifySelf: 'center' }}>{ptr('head', consumer, 'var(--fg-1)')}</div>
              <div style={{ gridRow: 3, gridColumn: tail + 1, justifySelf: 'center' }}>{ptr('tail', producer, 'var(--accent-fg)')}</div>
            </>}
        <div aria-hidden="true" style={{ position: 'absolute', left: `calc(100% / ${capacity} / 2)`, right: `calc(100% / ${capacity} / 2)`, bottom: 4, height: 10, border: '1px solid var(--fg-3)', borderTop: 0 }}>
          <span style={{ position: 'absolute', left: -4.5, top: -4, width: 0, height: 0, borderLeft: '4px solid transparent', borderRight: '4px solid transparent', borderBottom: '5px solid var(--fg-3)' }} />
        </div>
      </div>
    </div>
  );
}
