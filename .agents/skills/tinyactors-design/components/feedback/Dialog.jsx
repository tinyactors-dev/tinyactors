import React from 'react';
import { IconButton } from '../core/IconButton.jsx';
export function Dialog({ open = true, title, children, footer, onClose, inline, width }) {
  if (!open) return null;
  const box = (
    <div className="ta-dialog" role="dialog" aria-modal="true" style={width ? { maxWidth: width } : undefined} onClick={e => e.stopPropagation()}>
      <div className="ta-dialog__head">
        <span className="ta-dialog__title">{title}</span>
        {onClose && <IconButton icon="x" label="Close" size="sm" onClick={onClose} />}
      </div>
      <div className="ta-dialog__body">{children}</div>
      {footer && <div className="ta-dialog__foot">{footer}</div>}
    </div>
  );
  if (inline) return box;
  return <div className="ta-overlay" onClick={onClose}>{box}</div>;
}
