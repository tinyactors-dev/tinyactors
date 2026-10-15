import React from 'react';
import { Icon } from '../core/Icon.jsx';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Select({ label, hint, options = [], size = 'md', disabled, id, className, style, ...rest }) {
  const fid = id || (label ? 'sel-' + String(label).toLowerCase().replace(/\W+/g, '-') : undefined);
  return (
    <div className={cx('ta-field', className)} style={style}>
      {label && <label className="ta-label" htmlFor={fid}>{label}</label>}
      <div className={cx('ta-control', size !== 'md' && 'ta-control--' + size, disabled && 'ta-control--disabled')}>
        <select id={fid} disabled={disabled} {...rest}>
          {options.map(o => typeof o === 'string' ? <option key={o} value={o}>{o}</option> : <option key={o.value} value={o.value}>{o.label}</option>)}
        </select>
        <Icon name="chevrons-up-down" size={size === 'sm' ? 14 : 16} style={{ color: 'var(--fg-3)', pointerEvents: 'none' }} />
      </div>
      {hint && <span className="ta-hint">{hint}</span>}
    </div>
  );
}
