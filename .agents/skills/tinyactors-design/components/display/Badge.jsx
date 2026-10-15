import React from 'react';
const MAP = {
  running: ['var(--state-running)', 'var(--state-running-bg)'],
  waiting: ['var(--state-waiting)', 'var(--state-waiting-bg)'],
  done: ['var(--state-done)', 'var(--state-done-bg)'],
  error: ['var(--state-error)', 'var(--state-error-bg)'],
  stopped: ['var(--state-stopped)', 'var(--state-stopped-bg)'],
  neutral: ['var(--fg-2)', 'var(--surface-3)'],
};
export function Badge({ tone = 'neutral', dot = true, children, style, ...rest }) {
  const [c, b] = MAP[tone] || MAP.neutral;
  return (
    <span className={'ta-badge ta-badge--' + tone + (children == null ? ' ta-badge--dot' : '')} title={children == null ? tone : undefined} aria-label={children == null ? tone : undefined} style={{ '--c': c, '--b': b, ...style }} {...rest}>
      {dot && <span className="ta-badge__dot" />}
      {children}
    </span>
  );
}
