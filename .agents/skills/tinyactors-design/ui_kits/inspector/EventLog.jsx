(() => {
const { Tabs, Badge, Switch } = window.TinyactorsDesignSystem_2f489e;
function EventLog({ log, state, actor }) {
  const [tab, setTab] = React.useState('events');
  return (
    <aside style={{ width: 340, flex: 'none', borderLeft: '1px solid var(--border-1)', background: 'var(--surface-1)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ padding: '0 8px' }}><Tabs value={tab} onChange={setTab} items={[{ value: 'events', label: 'Events', count: log.length }, { value: 'state', label: 'State' }, { value: 'mailbox', label: 'Mailbox', count: 0 }]} /></div>
      {tab === 'events' && <>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '10px 12px' }}><Switch label="Auto-scroll" defaultChecked /><span style={{ font: '400 11px var(--font-mono)', color: 'var(--fg-4)' }}>t = virtual</span></div>
        <div style={{ overflow: 'auto', flex: 1, font: '400 12px/1 var(--font-mono)' }}>
          {log.slice().reverse().map((r, i) => (
            <div key={log.length - i} style={{ display: 'grid', gridTemplateColumns: '52px 1fr', gap: 8, padding: '9px 12px', borderTop: '1px solid var(--border-1)', background: i === 0 ? 'color-mix(in srgb, var(--accent) 8%, transparent)' : 'transparent' }}>
              <span style={{ color: 'var(--fg-4)' }}>{r.t}</span>
              <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                <span style={{ color: r.ok ? 'var(--fg-1)' : 'var(--state-error)' }}><span style={{ color: 'var(--accent-fg)' }}>→ </span>{r.ev}</span>
                <span style={{ color: 'var(--fg-3)' }}>{r.ok ? r.from + ' ⇢ ' + r.to : 'ignored in ' + r.from}</span>
              </div>
            </div>
          ))}
        </div>
      </>}
      {tab !== 'events' && <pre style={{ margin: 0, padding: 16, font: 'var(--type-code)', color: 'var(--fg-2)' }}>{tab === 'state' ? JSON.stringify({ actor, configuration: ['checkout', state], history: {}, data: { items: 2, attempts: log.filter(l => l.ev === 'RETRY').length } }, null, 2) : '// mailbox empty'}</pre>}
    </aside>
  );
}
window.EventLog = EventLog;
})();
