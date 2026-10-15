(() => {
const { Tag, Icon } = window.TinyactorsDesignSystem_2f489e;
const HALLMARKS = [
  ['boxes', 'Millions of actors', 'Run millions of actors in a single system. An idle actor costs its state and an empty mailbox, nothing more.'],
  ['memory-stick', 'Transparent paging', 'Actors page in and out of RAM transparently. Cold actors move to disk and wake on their next event with the same state.'],
  ['gauge', 'Hundreds of thousands of events/s', 'The scheduler handles hundreds of thousands of events per second, each processed to completion.'],
  ['activity', 'OpenTelemetry built in', 'Every send, transition and invoke emits OpenTelemetry spans out of the box. Point it at your collector.'],
];
const FEATURES = [
  ['file-check-2', 'Defined semantics', 'Event processing follows the SCXML algorithm: run-to-completion, document-order transitions, explicit entry and exit sets. No undefined behavior between C and JavaScript.'],
  ['languages', 'C and JavaScript', 'One C ABI, no runtime of its own. The JavaScript package is a thin wrapper, so an actor spawned from Node or Bun behaves exactly like one spawned from C.'],
  ['timer', 'Fast verify loops', 'Deterministic scheduler and virtual clock. Replay a trace, step one event, assert the configuration. Tests finish in milliseconds.'],
  ['bot', 'Legible to agents', 'Charts are data. State, mailbox and history serialize to JSON, so a coding agent can read a failure and propose the fix.'],
];
function Features() {
  return (
    <section style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '0 24px' }}>
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(220px,1fr))', borderLeft: '1px solid var(--border-1)' }}>
        {[...HALLMARKS, ...FEATURES].map(([ic, t, d]) => (
          <div key={t} style={{ padding: '40px 24px', borderRight: '1px solid var(--border-1)', borderBottom: '1px solid var(--border-1)' }}>
            <Icon name={ic} size={20} style={{ color: 'var(--accent-fg)' }} />
            <h3 style={{ font: 'var(--type-h3)', marginTop: 20 }}>{t}</h3>
            <p style={{ font: 'var(--type-body-sm)', color: 'var(--fg-2)', marginTop: 8 }}>{d}</p>
          </div>
        ))}
      </div>
    </section>
  );
}
function Bindings() {
  const langs = ['C', 'JavaScript · Node', 'JavaScript · Bun'];
  return (
    <section style={{ borderTop: '1px solid var(--border-1)', borderBottom: '1px solid var(--border-1)' }}>
      <div style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '28px 24px', display: 'flex', alignItems: 'center', gap: 24 }}>
        <span style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-3)', flex: 'none' }}>supported</span>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>{langs.map(l => <Tag key={l}>{l}</Tag>)}</div>
      </div>
    </section>
  );
}
function SiteFooter() {
  return (
    <footer style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '40px 24px 56px', display: 'flex', gap: 24, alignItems: 'center', font: '400 12px var(--font-mono)', color: 'var(--fg-3)' }}>
      <span style={{ font: '500 14px var(--font-mono)', letterSpacing: '-0.04em', color: 'var(--fg-2)' }}>tinyactors</span>
      <span>MIT</span><span>Docs</span><span>Semantics</span><span>GitHub</span>
      <span style={{ marginLeft: 'auto' }}>v0.4.2</span>
    </footer>
  );
}
Object.assign(window, { Features, Bindings, SiteFooter });
})();
