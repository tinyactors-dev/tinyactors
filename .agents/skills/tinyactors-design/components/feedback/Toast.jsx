import React from 'react';
import { IconButton } from '../core/IconButton.jsx';
const C = { info: 'var(--state-running)', success: 'var(--state-done)', warning: 'var(--state-waiting)', error: 'var(--state-error)' };
export function Toast({ tone = 'info', title, children, action, onClose, style }) {
  return (
    <div className="ta-toast" role="status" style={{ '--c': C[tone], ...style }}>
      <span className="ta-toast__mark" />
      <div style={{ flex: 1, minWidth: 0 }}>
        {title && <div className="ta-toast__title">{title}</div>}
        {children && <div className="ta-toast__msg">{children}</div>}
        {action && <div style={{ marginTop: 10 }}>{action}</div>}
      </div>
      {onClose && <IconButton icon="x" label="Dismiss" size="sm" onClick={onClose} />}
    </div>
  );
}
