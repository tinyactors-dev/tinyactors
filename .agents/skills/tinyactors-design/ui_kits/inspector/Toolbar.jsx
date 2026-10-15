(() => {
const { IconButton, Button, Badge, Kbd, Tooltip } = window.TinyactorsDesignSystem_2f489e;
function Toolbar({ paused, setPaused, onReset, step }) {
  return (
    <header style={{ height: 48, flex: 'none', display: 'flex', alignItems: 'center', gap: 16, padding: '0 12px 0 16px', borderBottom: '1px solid var(--border-1)', background: 'var(--bg-app)' }}>
      <span style={{ font: '500 15px/1 var(--font-mono)', letterSpacing: '-0.04em', flex: 'none' }}>tinyactors</span>
      <span style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-3)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', minWidth: 0 }}>inspector · <span style={{ color: 'var(--fg-1)' }}>shop-dev</span> · pid 48213</span>
      <Badge tone={paused ? 'waiting' : 'running'}>{paused ? 'paused' : 'live'}</Badge>
      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4 }}>
        <span style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-4)', marginRight: 8, whiteSpace: 'nowrap' }}>step {step}</span>
        <Tooltip side="bottom" content="Pause scheduler"><IconButton icon={paused ? 'play' : 'pause'} label={paused ? 'Resume' : 'Pause'} onClick={() => setPaused(!paused)} /></Tooltip>
        <Tooltip side="bottom" content="Step one event"><IconButton icon="skip-forward" label="Step" disabled={!paused} /></Tooltip>
        <Tooltip side="bottom" content="Reset to initial"><IconButton icon="rotate-ccw" label="Reset" onClick={onReset} /></Tooltip>
        <span style={{ width: 1, height: 20, background: 'var(--border-1)', margin: '0 8px' }} />
        <Button size="sm" icon="download">Export trace</Button>
        <span style={{ display: 'flex', gap: 3, marginLeft: 8 }}><Kbd>⌘</Kbd><Kbd>K</Kbd></span>
      </div>
    </header>
  );
}
window.Toolbar = Toolbar;
})();
