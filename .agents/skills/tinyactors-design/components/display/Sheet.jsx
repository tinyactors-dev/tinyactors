import React from 'react';
const cx = (...a) => a.filter(Boolean).join(' ');
export function Sheet({ children, grid = false, marks = true, parallax = false, className, style, bodyStyle, ...rest }) {
  const ref = React.useRef(null);
  React.useEffect(() => {
    if (!parallax || !grid || !ref.current) return;
    const el = ref.current, mq = matchMedia('(prefers-reduced-motion: reduce)');
    let raf = 0;
    const upd = () => { raf = 0; if (mq.matches) { el.style.removeProperty('--ta-py'); return; } el.style.setProperty('--ta-py', (-el.getBoundingClientRect().top * 0.5).toFixed(1) + 'px'); };
    const on = () => { if (!raf) raf = requestAnimationFrame(upd); };
    upd(); window.addEventListener('scroll', on, { passive: true }); window.addEventListener('resize', on);
    return () => { window.removeEventListener('scroll', on); window.removeEventListener('resize', on); cancelAnimationFrame(raf); };
  }, [parallax, grid]);
  return (
    <div ref={ref} className={cx('ta-sheet', grid && 'ta-sheet--grid', grid && parallax && 'ta-sheet--parallax', !marks && 'ta-sheet--flush', className)} style={style} {...rest}>
      {marks && ['tl', 'tr', 'bl', 'br'].map((p) => <span key={p} className={'ta-sheet__reg ta-sheet__reg--' + p} aria-hidden="true"><i /></span>)}
      <div className="ta-sheet__frame"><div className="ta-sheet__body" style={bodyStyle}>{children}</div></div>
    </div>
  );
}
