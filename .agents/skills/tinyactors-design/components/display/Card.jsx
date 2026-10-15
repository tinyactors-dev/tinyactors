import React from 'react';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Card({ title, eyebrow, actions, children, interactive, padded = true, generations = 0, className, ...rest }) {
  const card = (
    <div className={cx('ta-card', interactive && 'ta-card--interactive', className)} {...rest}>
      {(title || eyebrow || actions) && (
        <div className="ta-card__head">
          <div style={{ display: 'flex', flexDirection: 'column', gap: 6, minWidth: 0, flex: 1 }}>
            {eyebrow && <span className="ta-card__eyebrow">{eyebrow}</span>}
            {title && <span className="ta-card__title">{title}</span>}
          </div>
          {actions && <div style={{ display: 'flex', gap: 4 }}>{actions}</div>}
        </div>
      )}
      <div className={padded ? 'ta-card__body' : undefined}>{children}</div>
    </div>
  );
  if (!generations) return card;
  return (
    <div className="ta-gens" style={{ '--n': generations }}>
      {Array.from({ length: generations }, (_, k) => <span key={k} className="ta-gens__sheet" style={{ '--i': generations - k }} aria-hidden="true" />)}
      <div className="ta-gens__front">{card}<span className="ta-gens__mark ta-gens__mark--tl" aria-hidden="true" /><span className="ta-gens__mark ta-gens__mark--br" aria-hidden="true" /></div>
    </div>
  );
}
