import React from 'react';
import { Icon } from '../core/Icon.jsx';

const PALETTE = ['var(--data-1)', 'var(--data-2)', 'var(--data-3)', 'var(--data-4)', 'var(--data-5)', 'var(--data-6)'];
const hash = (s) => { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return Math.abs(h); };

export function formatDuration(ms) {
  if (ms < 1) return Math.round(ms * 1000) + 'µs';
  if (ms < 10) return ms.toFixed(2) + 'ms';
  if (ms < 1000) return ms.toFixed(ms < 100 ? 1 : 0) + 'ms';
  return (ms / 1000).toFixed(ms < 10000 ? 2 : 1) + 's';
}

function buildTree(spans) {
  const byId = new Map(spans.map((s) => [s.spanId, { ...s, children: [] }]));
  const roots = [];
  for (const n of byId.values()) {
    const p = n.parentId && byId.get(n.parentId);
    (p ? p.children : roots).push(n);
  }
  const sort = (a) => { a.sort((x, y) => x.start - y.start); a.forEach((n) => sort(n.children)); };
  sort(roots);
  return roots;
}

export function SpanTree({ spans = [], selectedId, onSelect, defaultCollapsed = [], serviceColors = {}, treeWidth = 320, rowHeight = 26, ticks = 4, showRuler = true, style, ...rest }) {
  const [collapsed, setCollapsed] = React.useState(() => new Set(defaultCollapsed));
  const roots = React.useMemo(() => buildTree(spans), [spans]);
  const t0 = spans.length ? Math.min(...spans.map((s) => s.start)) : 0;
  const t1 = spans.length ? Math.max(...spans.map((s) => s.start + s.duration)) : 1;
  const total = Math.max(t1 - t0, 1e-6);
  const color = (svc) => serviceColors[svc] || PALETTE[hash(svc || '') % PALETTE.length];
  const toggle = (id) => setCollapsed((c) => { const n = new Set(c); n.has(id) ? n.delete(id) : n.add(id); return n; });

  const rows = [];
  const walk = (nodes, depth, guides) => nodes.forEach((n, i) => {
    const last = i === nodes.length - 1;
    rows.push({ n, depth, guides, last });
    if (n.children.length && !collapsed.has(n.spanId)) walk(n.children, depth + 1, [...guides, !last]);
  });
  walk(roots, 0, []);

  const grid = `repeating-linear-gradient(to right, var(--border-1) 0 1px, transparent 1px ${100 / ticks}%)`;
  const INDENT = 14;

  return (
    <div role="tree" className="ta-spantree" style={{ font: '400 12px/1 var(--font-mono)', color: 'var(--fg-1)', background: 'var(--surface-1)', border: '1px solid var(--border-1)', borderRadius: 'var(--radius-2)', overflow: 'hidden', minWidth: 0, ...style }} {...rest}>
      {showRuler && (
        <div style={{ display: 'grid', gridTemplateColumns: `${treeWidth}px minmax(0,1fr)`, height: 28, borderBottom: '1px solid var(--border-1)', background: 'var(--surface-2)', color: 'var(--fg-4)', font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase' }}>
          <div style={{ display: 'flex', alignItems: 'center', padding: '0 12px', borderRight: '1px solid var(--border-1)' }} />
          <div style={{ position: 'relative', margin: '0 56px 0 8px' }}>
            {Array.from({ length: ticks + 1 }, (_, i) => (
              <span key={i} style={{ position: 'absolute', top: 0, bottom: 0, left: `${(i / ticks) * 100}%`, display: 'flex', alignItems: 'center', paddingLeft: 4, borderLeft: '1px solid var(--border-2)', whiteSpace: 'nowrap', textTransform: 'none' }}>{formatDuration((total * i) / ticks)}</span>
            ))}
          </div>
        </div>
      )}
      {rows.map(({ n, depth, guides, last }) => {
        const sel = n.spanId === selectedId;
        const err = n.status === 'error';
        const c = err ? 'var(--state-error)' : color(n.service);
        const left = ((n.start - t0) / total) * 100;
        const width = (n.duration / total) * 100;
        const hasKids = n.children.length > 0;
        const isCollapsed = collapsed.has(n.spanId);
        return (
          <div key={n.spanId} role="treeitem" aria-selected={sel} aria-expanded={hasKids ? !isCollapsed : undefined} aria-level={depth + 1} tabIndex={0}
            className={'ta-spantree__row' + (sel ? ' is-selected' : '')}
            onClick={() => onSelect && onSelect(n.spanId, n)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onSelect && onSelect(n.spanId, n); }
              if (hasKids && ((e.key === 'ArrowLeft' && !isCollapsed) || (e.key === 'ArrowRight' && isCollapsed))) { e.preventDefault(); toggle(n.spanId); }
            }}
            style={{ display: 'grid', gridTemplateColumns: `${treeWidth}px minmax(0,1fr)`, height: rowHeight, cursor: 'pointer', background: sel ? 'var(--accent-subtle)' : undefined, boxShadow: sel ? 'inset 2px 0 0 var(--accent)' : undefined }}>
            <div style={{ display: 'flex', alignItems: 'center', minWidth: 0, paddingLeft: 6, paddingRight: 12, borderRight: '1px solid var(--border-1)' }}>
              {guides.map((g, i) => <span key={i} style={{ flex: 'none', width: INDENT, alignSelf: 'stretch', background: g ? 'linear-gradient(var(--border-2),var(--border-2)) 6px 0/1px 100% no-repeat' : undefined }} />)}
              {depth > 0 && <span style={{ flex: 'none', width: 0, alignSelf: 'stretch', position: 'relative' }}><span style={{ position: 'absolute', left: -INDENT + 6, top: 0, height: last ? '50%' : '100%', width: 1, background: 'var(--border-2)' }} /><span style={{ position: 'absolute', left: -INDENT + 6, top: '50%', width: 6, height: 1, background: 'var(--border-2)' }} /></span>}
              <span onClick={hasKids ? (e) => { e.stopPropagation(); toggle(n.spanId); } : undefined}
                style={{ flex: 'none', width: 16, height: 16, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--fg-3)', borderRadius: 'var(--radius-1)' }}>
                {hasKids && <Icon name="chevron-right" size={12} style={{ transform: isCollapsed ? 'none' : 'rotate(90deg)', transition: 'transform var(--dur-fast) var(--ease-step)' }} />}
              </span>
              <span style={{ flex: 'none', width: 3, height: 14, marginRight: 8, borderRadius: 1, background: c }} />
              <span style={{ flex: 'none', color: 'var(--fg-3)', marginRight: 6 }}>{n.service}</span>
              <span style={{ minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: err ? 'var(--state-error)' : 'var(--fg-1)', fontWeight: sel ? 500 : 400 }}>{n.name}</span>
              {err && <Icon name="alert-triangle" size={12} style={{ flex: 'none', marginLeft: 6, color: 'var(--state-error)' }} />}
            </div>
            <div style={{ position: 'relative', margin: '0 56px 0 8px', backgroundImage: grid }}>
              <div style={{ position: 'absolute', top: '50%', left: `${left}%`, width: `max(${width}%, 2px)`, height: 10, transform: 'translateY(-50%)', background: c, borderRadius: 1, opacity: sel ? 1 : 0.85 }}>
                {(n.events || []).map((ev, i) => (
                  <span key={i} title={ev.name} style={{ position: 'absolute', top: -2, bottom: -2, width: 1, left: `${n.duration ? ((ev.time - n.start) / n.duration) * 100 : 0}%`, background: 'var(--fg-1)' }} />
                ))}
              </div>
              <span className={'ta-spantree__dur' + (depth === 0 || err ? ' is-shown' : '')} style={{ position: 'absolute', top: '50%', transform: 'translateY(-50%)', whiteSpace: 'nowrap', color: 'var(--fg-3)', fontSize: 11, left: `calc(${left + width}% + 6px)` }}>{formatDuration(n.duration)}</span>
            </div>
          </div>
        );
      })}
    </div>
  );
}
