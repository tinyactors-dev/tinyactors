(() => {
const { Badge, Input, Icon } = window.TinyactorsDesignSystem_2f489e;
function ActorTree({ selected, onSelect, tones }) {
  return (
    <aside style={{ width: 260, flex: 'none', borderRight: '1px solid var(--border-1)', background: 'var(--surface-1)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ padding: 12, borderBottom: '1px solid var(--border-1)' }}><Input icon="search" placeholder="Filter actors" size="sm" mono /></div>
      <div style={{ padding: '10px 12px 6px', font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-4)' }}>actors · {window.INSPECTOR_TREE.length}</div>
      <div style={{ overflow: 'auto', padding: '0 6px 12px' }}>
        {window.INSPECTOR_TREE.map(a => {
          const sel = a.id === selected, tone = tones[a.id] || a.tone;
          return (
            <div key={a.id} onClick={() => onSelect(a.id)} style={{ display: 'flex', alignItems: 'center', gap: 8, height: 30, paddingLeft: 8 + a.depth * 16, paddingRight: 8, borderRadius: 'var(--radius-2)', cursor: 'pointer', background: sel ? 'var(--surface-3)' : 'transparent', color: sel ? 'var(--fg-1)' : 'var(--fg-2)' }}>
              <Icon name={a.kind === 'supervisor' ? 'network' : 'circle-dot'} size={14} style={{ color: 'var(--fg-4)' }} />
              <span style={{ font: '400 12px/1 var(--font-mono)', flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis' }}>{a.name}</span>
              {a.kind !== 'supervisor' && <Badge tone={tone} />}
            </div>
          );
        })}
      </div>
    </aside>
  );
}
window.ActorTree = ActorTree;
})();
