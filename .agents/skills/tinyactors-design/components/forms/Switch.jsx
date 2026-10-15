import React from 'react';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Switch({ label, disabled, className, ...rest }) {
  return (
    <label className={cx('ta-switch', disabled && 'ta-switch--disabled', className)}>
      <input type="checkbox" role="switch" disabled={disabled} {...rest} />
      <span className="ta-switch__track" />
      {label && <span>{label}</span>}
    </label>
  );
}
