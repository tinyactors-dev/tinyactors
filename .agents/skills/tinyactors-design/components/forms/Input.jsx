import React from 'react';
import { Icon } from '../core/Icon.jsx';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Input({ label, hint, error, prefix, icon, mono, size = 'md', disabled, id, className, style, ...rest }) {
  const fid = id || (label ? 'in-' + String(label).toLowerCase().replace(/\W+/g, '-') : undefined);
  return (
    <div className={cx('ta-field', className)} style={style}>
      {label && <label className="ta-label" htmlFor={fid}>{label}</label>}
      <div className={cx('ta-control', error && 'ta-control--error', mono && 'ta-control--mono', size !== 'md' && 'ta-control--' + size, disabled && 'ta-control--disabled')}>
        {icon && <Icon name={icon} size={size === 'sm' ? 14 : 16} style={{ color: 'var(--fg-3)' }} />}
        {prefix && <span className="ta-prefix">{prefix}</span>}
        <input id={fid} disabled={disabled} {...rest} />
      </div>
      {(error || hint) && <span className={cx('ta-hint', error && 'ta-hint--error')}>{error || hint}</span>}
    </div>
  );
}
