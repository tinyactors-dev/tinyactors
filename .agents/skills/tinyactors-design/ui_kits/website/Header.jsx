(() => {
const { Button, Badge, Icon } = window.TinyactorsDesignSystem_2f489e;
function SiteHeader({ page, go }) {
  const link = (id, label) => <a href="#" onClick={e => { e.preventDefault(); go(id); }} style={{ font: 'var(--type-ui)', color: page === id ? 'var(--fg-1)' : 'var(--fg-3)', textDecoration: 'none' }}>{label}</a>;
  return (
    <header style={{ position: 'sticky', top: 0, zIndex: 10, background: 'color-mix(in srgb, var(--bg-app) 88%, transparent)', backdropFilter: 'blur(8px)', borderBottom: '1px solid var(--border-1)' }}>
      <div style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '0 24px', height: 56, display: 'flex', alignItems: 'center', gap: 28 }}>
        <a href="#" onClick={e => { e.preventDefault(); go('home'); }} style={{ font: '500 18px/1 var(--font-mono)', letterSpacing: '-0.04em', color: 'var(--fg-1)', textDecoration: 'none' }}>tinyactors</a>
        <Badge dot={false}>v0.4.2</Badge>
        <nav style={{ display: 'flex', gap: 22, marginLeft: 8 }}>{link('docs', 'Docs')}{link('semantics', 'Semantics')}{link('bindings', 'Bindings')}{link('blog', 'Changelog')}</nav>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8, alignItems: 'center' }}>
          <Button variant="ghost" size="sm" icon="github">GitHub</Button>
          <Button variant="primary" size="sm" onClick={() => go('docs')}>Install</Button>
        </div>
      </div>
    </header>
  );
}
window.SiteHeader = SiteHeader;
})();
