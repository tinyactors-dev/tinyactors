import React from 'react';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Checkbox({ label, disabled, className, ...rest }) {
  return (
    <label className={cx('ta-check', disabled && 'ta-check--disabled', className)}>
      <input type="checkbox" disabled={disabled} {...rest} />
      <span className="ta-check__box" />
      {label && <span>{label}</span>}
    </label>
  );
}
