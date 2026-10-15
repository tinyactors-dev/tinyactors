import React from 'react';
import { Icon } from './Icon.jsx';
const cx = (...a) => a.filter(Boolean).join(' ');
export function IconButton({ icon, label, variant = 'ghost', size = 'md', className, ...rest }) {
  return (
    <button type="button" aria-label={label} title={label} className={cx('ta-btn', 'ta-btn--icon', 'ta-btn--' + variant, size !== 'md' && 'ta-btn--' + size, className)} {...rest}>
      {typeof icon === 'string' ? <Icon name={icon} size={size === 'sm' ? 14 : 16} /> : icon}
    </button>
  );
}
