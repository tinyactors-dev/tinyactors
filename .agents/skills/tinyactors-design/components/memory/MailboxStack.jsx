import React from 'react';

export function MailboxStack({ messages = [], depth = 6, width = 240, height = 260, style, ...rest }) {
  const [items, setItems] = React.useState(() => messages.slice(0, depth).map((m, i) => ({ m, st: 'idle', i })));
  React.useEffect(() => {
    const want = messages.slice(0, depth);
    const ids = new Set(want.map((m) => m.id));
    setItems((prev) => {
      const had = new Set(prev.filter((x) => x.st !== 'out').map((x) => x.m.id));
      const now = want.map((m, i) => ({ m, i, st: had.has(m.id) ? 'idle' : 'in' }));
      const leaving = prev.filter((x) => !ids.has(x.m.id)).map((x) => ({ ...x, st: 'out' }));
      return [...now, ...leaving];
    });
    let r2 = 0;
    const r1 = requestAnimationFrame(() => { r2 = requestAnimationFrame(() => setItems((p) => p.map((x) => (x.st === 'in' ? { ...x, st: 'idle' } : x)))); });
    const t = setTimeout(() => setItems((p) => p.filter((x) => x.st !== 'out')), 750);
    return () => { cancelAnimationFrame(r1); cancelAnimationFrame(r2); clearTimeout(t); };
  }, [messages, depth]);
  return (
    <div className="ta-mbox" style={{ height, ...style }} {...rest}>
      <div className="ta-mbox__stack">
        {items.map((x) => (
          <div key={x.m.id} className={'ta-mbox__msg is-' + x.st} style={{ '--i': x.i, width, left: -width / 2, zIndex: 100 - x.i }}>
            <span className="ta-mbox__row"><span>{x.m.type}</span><span style={{ color: 'var(--fg-3)' }}>#{x.m.id}</span></span>
            {x.m.note && <span className="ta-mbox__note">{x.m.note}</span>}
          </div>
        ))}
      </div>
    </div>
  );
}
