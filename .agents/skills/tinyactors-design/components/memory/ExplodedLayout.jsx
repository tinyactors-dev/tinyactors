import React from 'react';
import { FIELD_KINDS } from './MemoryLayout.jsx';

export function ExplodedLayout({ layers = [], exploded, defaultExploded = true, gap = 44, width = 240, height = 120, stageHeight = 300, style, ...rest }) {
  const [open, setOpen] = React.useState(defaultExploded);
  const ex = exploded ?? open;
  const n = layers.length;
  const g = ex ? gap : 4;
  return (
    <div className={'ta-explode' + (ex ? ' is-open' : '')} onClick={exploded == null ? () => setOpen((o) => !o) : undefined} style={{ height: stageHeight, ...style }} {...rest}>
      <div className="ta-explode__stage" style={{ width, height, transform: `rotateX(58deg) rotateZ(-36deg) translateZ(${-((n - 1) * g) / 2}px)` }}>
        <div className="ta-explode__shadow" />
        {layers.map((l, i) => {
          const total = l.fields.reduce((a, f) => a + f.size, 0) || 1;
          return (
            <div key={i} className="ta-explode__layer" style={{ transform: `translateZ(${(n - 1 - i) * g}px)` }}>
              {l.fields.map((f, j) => {
                const k = FIELD_KINDS[f.kind] || FIELD_KINDS.data;
                return <i key={j} title={`${f.name || 'padding'} · ${f.size} B`} style={{ flex: f.size, background: k.bg, color: k.fg }}>{f.size / total >= 0.18 ? f.name : ''}</i>;
              })}
            </div>
          );
        })}
      </div>
      {layers.map((l, i) => (
        <span key={i} className="ta-explode__lead" style={{ top: `calc(50% + ${(((n - 1) / 2 - (n - 1 - i)) * gap * 0.85).toFixed(1)}px)` }}>{l.name}</span>
      ))}
    </div>
  );
}
