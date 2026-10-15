import React from 'react';
const CDN = 'https://unpkg.com/lucide-static@0.460.0/icons/';
const cache = new Map();
function load(name) {
  if (!cache.has(name)) cache.set(name, fetch(CDN + name + '.svg').then((r) => (r.ok ? r.text() : '')).then((t) => t.replace(/\s(width|height)="[^"]*"/g, ' width="100%" height="100%"').replace(/<!--[\s\S]*?-->/g, '')).catch(() => ''));
  return cache.get(name);
}
export function Icon({ name, size = 16, strokeWidth, style, className, ...rest }) {
  const [svg, setSvg] = React.useState('');
  React.useEffect(() => { let on = true; load(name).then((t) => on && setSvg(strokeWidth ? t.replace(/stroke-width="[^"]*"/, 'stroke-width="' + strokeWidth + '"') : t)); return () => { on = false; }; }, [name, strokeWidth]);
  return <span aria-hidden="true" className={'ta-icon' + (className ? ' ' + className : '')} style={{ width: size, height: size, ...style }} dangerouslySetInnerHTML={{ __html: svg }} {...rest} />;
}
