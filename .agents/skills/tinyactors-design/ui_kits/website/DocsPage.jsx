(() => {
const { CodeBlock, Input, Tabs, Badge, Button } = window.TinyactorsDesignSystem_2f489e;
const DOC_NAV = [['Getting started', ['Install', 'First actor', 'Sending events']], ['Concepts', ['Statecharts', 'Mailboxes', 'Supervision', 'Timers']], ['Semantics', ['Event processing', 'Entry & exit sets', 'Conformance']], ['Bindings', ['C', 'JavaScript']]];
const SNIPS = {
  C: '#include <tinyactors.h>\n\nta_system *sys = ta_system_new(NULL);\nta_actor *door = ta_spawn(sys, &door_chart, NULL);\nta_send(door, "OPEN", NULL);\nassert(ta_in(door, "opened"));',
  JavaScript: 'import { System } from "tinyactors";\n\nconst sys = new System();\nconst door = sys.spawn(doorChart);\ndoor.send("OPEN");\nassert(door.in("opened"));',
};
function DocsPage() {
  const [cur, setCur] = React.useState('First actor');
  const [lang, setLang] = React.useState('C');
  return (
    <div style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '0 24px', display: 'grid', gridTemplateColumns: 'var(--sidebar-w) minmax(0,1fr) 180px', gap: 48 }}>
      <aside style={{ padding: '32px 0', borderRight: '1px solid var(--border-1)', paddingRight: 24, display: 'flex', flexDirection: 'column', gap: 24 }}>
        <Input icon="search" placeholder="Search docs" size="sm" />
        {DOC_NAV.map(([g, items]) => (
          <div key={g} style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
            <span style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-4)', marginBottom: 8 }}>{g}</span>
            {items.map(i => <a key={i} href="#" onClick={e => { e.preventDefault(); setCur(i); }} style={{ font: '400 14px/1 var(--font-sans)', padding: '7px 10px', borderRadius: 'var(--radius-2)', textDecoration: 'none', color: cur === i ? 'var(--fg-1)' : 'var(--fg-2)', background: cur === i ? 'var(--surface-2)' : 'transparent' }}>{i}</a>)}
          </div>
        ))}
      </aside>
      <article style={{ padding: '40px 0 80px', maxWidth: 'var(--prose-max)' }}>
        <div style={{ font: '400 12px var(--font-mono)', color: 'var(--fg-3)' }}>Getting started / {cur}</div>
        <h1 style={{ font: 'var(--type-h1)', letterSpacing: 'var(--tracking-display)', marginTop: 16 }}>{cur}</h1>
        <p style={{ font: 'var(--type-prose)', color: 'var(--fg-1)', marginTop: 20 }}>An actor is a statechart with a mailbox. You describe the chart once, spawn as many instances as you need, and talk to them only by sending events. Each actor processes one event at a time, to completion.</p>
        <h2 style={{ font: 'var(--type-h2)', marginTop: 40 }}>Spawn and send</h2>
        <p style={{ font: 'var(--type-prose)', color: 'var(--fg-1)', marginTop: 12 }}>The same four calls exist in C and JavaScript. <code style={{ font: '400 15px var(--font-mono)', color: 'var(--fg-1)', background: 'var(--surface-2)', padding: '2px 5px', borderRadius: 2 }}>send</code> enqueues; the scheduler delivers on the next step.</p>
        <div style={{ marginTop: 20 }}><Tabs variant="segmented" items={Object.keys(SNIPS)} value={lang} onChange={setLang} /></div>
        <CodeBlock style={{ marginTop: 10 }} filename={'door.' + ({ C: 'c', JavaScript: 'js' })[lang]} code={SNIPS[lang]} highlight={[5]} />
        <div style={{ marginTop: 28, padding: '14px 16px', border: '1px solid var(--border-1)', borderRadius: 'var(--radius-2)', background: 'var(--surface-1)', display: 'flex', gap: 12, alignItems: 'baseline' }}>
          <Badge tone="waiting" dot={false}>note</Badge>
          <p style={{ font: 'var(--type-body-sm)', color: 'var(--fg-2)' }}>Events sent from inside a transition are queued as internal events and processed before the next external one, as SCXML specifies.</p>
        </div>
        <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 48, paddingTop: 20, borderTop: '1px solid var(--border-1)' }}>
          <Button variant="ghost" icon="arrow-left">Install</Button>
          <Button variant="ghost" iconRight="arrow-right">Sending events</Button>
        </div>
      </article>
      <nav style={{ padding: '40px 0', display: 'flex', flexDirection: 'column', gap: 10, font: '400 13px var(--font-sans)', color: 'var(--fg-3)' }}>
        <span style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-4)' }}>on this page</span>
        <span style={{ color: 'var(--fg-1)' }}>Spawn and send</span><span>Asserting state</span><span>Next steps</span>
      </nav>
    </div>
  );
}
window.DocsPage = DocsPage;
})();
