import React from 'react';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Radio({ label, disabled, className, ...rest }) {
  return (
    <label className={cx('ta-check', 'ta-check--radio', disabled && 'ta-check--disabled', className)}>
      <input type="radio" disabled={disabled} {...rest} />
      <span className="ta-check__box" />
      {label && <span>{label}</span>}
    </label>
  );
}
