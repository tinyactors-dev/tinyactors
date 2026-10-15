import React from 'react';
export function Tabs({ items = [], value, defaultValue, onChange, variant = 'underline', style }) {
  const [inner, setInner] = React.useState(defaultValue ?? (items[0] && (items[0].value ?? items[0])));
  const cur = value ?? inner;
  const pick = v => { setInner(v); onChange && onChange(v); };
  return (
    <div role="tablist" className={'ta-tabs' + (variant === 'segmented' ? ' ta-tabs--segmented' : '')} style={style}>
      {items.map(it => {
        const v = it.value ?? it, l = it.label ?? it;
        return (
          <button key={v} role="tab" aria-selected={cur === v} className="ta-tab" onClick={() => pick(v)}>
            {l}{it.count != null && <span className="ta-tab__count">{it.count}</span>}
          </button>
        );
      })}
    </div>
  );
}
