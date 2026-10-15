(() => {
const { Button, CodeBlock, Tag, Sheet } = window.TinyactorsDesignSystem_2f489e;
const HERO_CODE = 'import { System } from "tinyactors";\n\nconst sys = new System();\nconst door = sys.spawn(doorChart);\n\ndoor.send("OPEN");\nassert(door.in("opened"));';
function Hero({ go }) {
  const events = { closed: ['OPEN', 'LOCK'], opened: ['CLOSE'], locked: ['UNLOCK'] };
  const next = { OPEN: 'opened', CLOSE: 'closed', LOCK: 'locked', UNLOCK: 'closed' };
  const [s, setS] = React.useState('closed');
  return (
    <section style={{ borderBottom: '1px solid var(--border-1)' }}>
      <div style={{ maxWidth: 'var(--content-max)', margin: '0 auto', padding: '96px 24px 88px', display: 'grid', gridTemplateColumns: 'minmax(0,1.05fr) minmax(0,1fr)', gap: 56, alignItems: 'center' }}>
        <div>
          <div style={{ font: 'var(--type-label)', letterSpacing: 'var(--tracking-label)', textTransform: 'uppercase', color: 'var(--fg-3)', display: 'flex', alignItems: 'center', gap: 10 }}><span style={{ width: 28, height: 1, background: 'var(--fg-3)' }} />embeddable actor runtime</div>
          <h1 style={{ font: '400 80px/0.98 var(--font-display)', letterSpacing: 'var(--tracking-display)', margin: '20px 0 0' }}>Actors,<br />as a <em>library</em>.</h1>
          <p style={{ font: '400 18px/1.55 var(--font-sans)', color: 'var(--fg-2)', marginTop: 24, maxWidth: 500 }}>A small C-ABI runtime for supervised state machines. Supervision from Elixir, statecharts from XState, semantics from SCXML. Use it from C or JavaScript (Node and Bun).</p>
          <div style={{ display: 'flex', gap: 10, marginTop: 32 }}>
            <Button variant="primary" size="lg" iconRight="arrow-right" onClick={() => go('docs')}>Get started</Button>
            <Button size="lg" icon="book-open" onClick={() => go('docs')}>Read the semantics</Button>
          </div>
          <div style={{ marginTop: 28, font: '400 13px var(--font-mono)', color: 'var(--fg-3)' }}>$ npm install tinyactors</div>
        </div>
        <Sheet grid parallax bodyStyle={{ padding: 20, display: 'flex', flexDirection: 'column', gap: 16 }}>
          <CodeBlock filename="door.test.js" highlight={[6, 7]} code={HERO_CODE} />
          <div style={{ padding: '8px 4px 0', display: 'flex', flexDirection: 'column', gap: 16 }}>
            <div style={{ display: 'flex', alignItems: 'baseline', gap: 10 }}>
              <span style={{ font: '400 11px var(--font-mono)', color: 'var(--fg-4)' }}>state</span>
              <span style={{ font: '500 16px var(--font-mono)', color: 'var(--accent-fg)' }}>{s}</span>
            </div>
            <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
              <span style={{ font: '400 11px var(--font-mono)', color: 'var(--fg-4)' }}>send</span>
              {events[s].map(e => <Tag key={e} glyph="→" style={{ cursor: 'pointer' }} onClick={() => setS(next[e])}>{e}</Tag>)}
            </div>
          </div>
        </Sheet>
      </div>
    </section>
  );
}
window.Hero = Hero;
})();
