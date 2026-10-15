import React from 'react';
import { Icon } from './Icon.jsx';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Button({ variant = 'secondary', size = 'md', icon, iconRight, children, className, type = 'button', ...rest }) {
  const is = size === 'sm' ? 14 : 16;
  return (
    <button type={type} className={cx('ta-btn', 'ta-btn--' + variant, size !== 'md' && 'ta-btn--' + size, className)} {...rest}>
      {icon && (typeof icon === 'string' ? <Icon name={icon} size={is} /> : icon)}
      {children}
      {iconRight && (typeof iconRight === 'string' ? <Icon name={iconRight} size={is} /> : iconRight)}
    </button>
  );
}
