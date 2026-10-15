(() => {
const { Tag, Input, Button, Tabs, Sheet } = window.TinyactorsDesignSystem_2f489e;
function ChartView({ actor, state, onSend }) {
  const C = window.INSPECTOR_CHART;
  const [ev, setEv] = React.useState('');
  const avail = Object.keys(C.on[state] || {});
  return (
    <section style={{ flex: 1, minWidth: 0, overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '0 16px', height: 44, flex: 'none', whiteSpace: 'nowrap', overflow: 'hidden', borderBottom: '1px solid var(--border-1)' }}>
        <span style={{ font: '500 13px var(--font-mono)', flex: 'none' }}>{actor}</span>
        <span style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-4)', minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis' }}>chart: {C.id}.scxml</span>
        <div style={{ marginLeft: 'auto', flex: 'none' }}><Tabs variant="segmented" items={['Chart', 'Tree', 'JSON']} /></div>
      </div>
      <Sheet grid style={{ flex: 1, minHeight: 0 }} bodyStyle={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 10, padding: 24 }}>
        <span style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-4)' }}>current state</span>
        <span style={{ font: '500 28px/1 var(--font-mono)', color: 'var(--accent-fg)' }}>{state}</span>
        <span style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-4)' }}>statechart view: component pending</span>
      </Sheet>
      <div style={{ borderTop: '1px solid var(--border-1)', padding: 12, display: 'flex', flexWrap: 'wrap', gap: 10, alignItems: 'center', flex: 'none', background: 'var(--surface-1)' }}>
        <span style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-4)' }}>send</span>
        {avail.map(e => <Tag key={e} glyph="→" style={{ cursor: 'pointer' }} onClick={() => onSend(e)}>{e}</Tag>)}
        {!avail.length && <span style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-4)' }}>final — no transitions</span>}
        <form style={{ marginLeft: 'auto', display: 'flex', gap: 8, minWidth: 0 }} onSubmit={e => { e.preventDefault(); if (ev) { onSend(ev.toUpperCase()); setEv(''); } }}>
          <Input mono size="sm" placeholder="EVENT.NAME" value={ev} onChange={e => setEv(e.target.value)} style={{ width: 180, maxWidth: '100%', minWidth: 0 }} />
          <Button type="submit" size="sm" variant="primary" icon="send">Send</Button>
        </form>
      </div>
    </section>
  );
}
window.ChartView = ChartView;
})();
