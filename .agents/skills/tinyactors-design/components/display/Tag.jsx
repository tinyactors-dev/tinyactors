import React from 'react';
import { Icon } from '../core/Icon.jsx';
export function Tag({ children, glyph, onRemove, ...rest }) {
  return (
    <span className={'ta-tag' + (onRemove ? ' ta-tag--removable' : '')} {...rest}>
      {glyph && <span className="ta-tag__glyph">{glyph}</span>}
      {children}
      {onRemove && <button aria-label="Remove" onClick={onRemove}><Icon name="x" size={12} /></button>}
    </span>
  );
}
