<script setup lang="ts">
import { withBase } from 'vitepress';

const guarantees = [
  { n: '01', title: 'The value JavaScript gives', text: 'Or a fault that says why. Never a different value: where JavaScript would coerce, compare identities or look on a prototype, staticjs stops.' },
  { n: '02', title: 'Every evaluation ends', text: 'No loops but the built-ins over collections, no recursion. A program runs on fuel the host pushes, pauses where it runs out, and resumes.' },
  { n: '03', title: 'No side effects', text: 'A program reaches only what the host passes in. No I/O, no global state, no clock of its own: even Date.now() and Math.random() are the host’s.' },
  { n: '04', title: 'Deterministic', text: 'The same program on the same inputs gives the same value and the same fuel on every machine, down to the last bit of Math.sin.' },
  { n: '05', title: 'Bounded memory', text: 'Memory comes from the host’s allocator, every construct has a limit, and nothing recurses, so no program can overflow the C stack.' },
  { n: '06', title: 'Freestanding', text: 'No C library. One C file builds for Linux, macOS and WebAssembly from the same source, and the wasm module imports nothing.' },
];

const ways = [
  { title: 'A C library', tag: 'any compiler', code: 'cc -std=c11 -Iinclude app.c lib/libstaticjs.a', text: 'Static and shared libraries for Linux and macOS, x86_64 and arm64.', link: '/guide/getting-started' },
  { title: 'One C file', tag: 'clang', code: 'clang -O2 -I. app.c staticjs.c', text: 'The amalgamation: the whole library as staticjs.c and staticjs.h.', link: '/guide/getting-started#_3-build-it' },
  { title: 'WebAssembly', tag: 'browser · Node', code: "await WebAssembly.instantiate(bytes, {})", text: 'staticjs.wasm imports nothing and brings its own allocator.', link: '/guide/wasm' },
];

const stops = [
  { src: '[1, 2] === [1, 2]', js: 'false', sjs: 'fault: === on two objects compares identity, which staticjs does not have' },
  { src: '"5" * 2', js: '10', sjs: 'fault: arithmetic in staticjs is on numbers' },
  { src: '({}).a.b', js: 'TypeError', sjs: 'fault: cannot read a property of undefined' },
  { src: '() => { for (;;) {} }', js: 'never ends, if called', sjs: 'refused when compiled: staticjs has no loops' },
];
</script>

<template>
  <div class="home">
    <section class="hero">
      <div class="copy">
        <p class="eyebrow">A pure, total subset of JavaScript</p>
        <h1>JavaScript that <em>always ends</em>, and never surprises.</h1>
        <p class="lede">
          staticjs evaluates programs written by someone else (guards, rules, filters, computed fields) and gives
          exactly the value JavaScript would, or a fault that says why. It is a freestanding C library, and a
          WebAssembly module for the browser.
        </p>
        <div class="actions">
          <a class="sjs-button primary big" :href="withBase('/guide/getting-started')">Get started</a>
          <a class="sjs-button big" :href="withBase('/explore/pipeline')">Watch a program run</a>
        </div>
        <p class="install"><span class="prompt">$</span> clang -O2 -I. app.c staticjs.c</p>
      </div>
      <div class="demo">
        <p class="demo-label">Edit it: this runs in your browser, through staticjs.wasm.</p>
        <div class="vp-doc"><slot name="demo" /></div>
      </div>
    </section>

    <section class="band">
      <p class="eyebrow">What it guarantees</p>
      <h2>Six promises, held by its test suite.</h2>
      <div class="grid">
        <article v-for="g in guarantees" :key="g.n" class="card">
          <span class="n">{{ g.n }}</span>
          <h3>{{ g.title }}</h3>
          <p>{{ g.text }}</p>
        </article>
      </div>
    </section>

    <section class="band">
      <p class="eyebrow">Where it stops</p>
      <h2>A fault is never a different value.</h2>
      <div class="stops">
        <div class="row head"><span>Program</span><span>JavaScript</span><span>staticjs</span></div>
        <div v-for="s in stops" :key="s.src" class="row">
          <code>{{ s.src }}</code>
          <span class="js">{{ s.js }}</span>
          <span class="sjs"><span class="chip error">stop</span> {{ s.sjs }}</span>
        </div>
      </div>
      <p class="more"><a :href="withBase('/guide/faults')">Every place it stops, and why →</a></p>
    </section>

    <section class="band">
      <p class="eyebrow">Three ways in</p>
      <h2>The same library, wherever it runs.</h2>
      <div class="ways">
        <a v-for="w in ways" :key="w.title" class="card way" :href="withBase(w.link)">
          <span class="tag">{{ w.tag }}</span>
          <h3>{{ w.title }}</h3>
          <p>{{ w.text }}</p>
          <code>{{ w.code }}</code>
        </a>
      </div>
    </section>

    <section class="band pipeline-teaser">
      <div>
        <p class="eyebrow">Inside</p>
        <h2>Every stage is pushed, none recurses.</h2>
        <p class="lede small">
          Bytes into the parser, events into the compiler, fuel into the evaluator, buffer space into the writer. Nothing
          blocks or calls back for more input, so a host decides when each step happens.
        </p>
        <a class="sjs-button primary big" :href="withBase('/explore/pipeline')">Open the pipeline explorer</a>
      </div>
      <ol class="mini">
        <li><span>01</span>Parser<small>bytes → events</small></li>
        <li><span>02</span>Compiler<small>events → program</small></li>
        <li><span>03</span>Evaluator<small>fuel → value</small></li>
        <li><span>04</span>Writer<small>value → text</small></li>
      </ol>
    </section>

  </div>
</template>

<style scoped>
.home {
  max-width: 1200px;
  margin: 0 auto;
  padding: 0 24px 96px;
}
.hero {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: 48px;
  align-items: center;
  padding: 72px 0 56px;
}
@media (max-width: 960px) {
  .hero {
    grid-template-columns: minmax(0, 1fr);
    padding-top: 40px;
    gap: 32px;
  }
}
h1 {
  font: var(--fw-regular) clamp(40px, 6vw, 68px) / 1.02 var(--font-serif);
  letter-spacing: var(--tracking-display);
  margin: 0 0 24px;
  color: var(--fg-1);
  text-wrap: balance;
}
h1 em {
  font-style: italic;
  color: var(--accent-fg);
}
.lede {
  font: var(--type-prose);
  font-size: 19px;
  color: var(--fg-2);
  margin: 0 0 28px;
  max-width: 560px;
}
.lede.small {
  font-size: 17px;
}
.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-bottom: 24px;
}
.sjs-button.big {
  height: var(--control-h-lg);
  padding: 0 20px;
  font-size: 14px;
}
.install {
  display: inline-flex;
  gap: 10px;
  margin: 0;
  padding: 8px 14px;
  font: var(--type-code);
  color: var(--code-fg);
  background: var(--surface-code);
  border: 1px solid var(--code-border);
  border-radius: var(--radius-2);
  max-width: 100%;
  overflow-x: auto;
  white-space: nowrap;
}
.prompt {
  color: var(--accent-fg);
  user-select: none;
}
.demo {
  min-width: 0;
}
.demo-label {
  font: var(--type-label);
  letter-spacing: 0.04em;
  color: var(--fg-3);
  margin: 0 0 -8px;
}
.band {
  padding: 56px 0 0;
  border-top: 1px solid var(--border-1);
  margin-top: 40px;
}
h2 {
  font: var(--type-h2);
  font-size: clamp(26px, 3vw, 34px);
  letter-spacing: var(--tracking-tight);
  margin: 0 0 28px;
  color: var(--fg-1);
  max-width: 760px;
  text-wrap: balance;
}
.grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
}
@media (max-width: 900px) {
  .grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
@media (max-width: 560px) {
  .grid {
    grid-template-columns: minmax(0, 1fr);
  }
}
.card {
  display: block;
  padding: 20px 22px;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  color: inherit;
  text-decoration: none;
}
.card .n {
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  color: var(--accent-fg);
}
.card h3 {
  font: var(--type-h3);
  margin: 10px 0 6px;
  color: var(--fg-1);
}
.card p {
  margin: 0;
  font-size: 14.5px;
  line-height: 1.6;
  color: var(--fg-2);
}
.stops {
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  background: var(--surface-1);
  overflow: hidden;
}
.row {
  display: grid;
  grid-template-columns: minmax(0, 1.2fr) minmax(0, 0.8fr) minmax(0, 2fr);
  gap: 16px;
  align-items: center;
  padding: 12px 18px;
  border-top: 1px solid var(--border-1);
  font-size: 14.5px;
}
.row.head {
  border-top: 0;
  background: var(--surface-2);
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  text-transform: uppercase;
  color: var(--fg-3);
}
.row code {
  font: var(--type-code);
  color: var(--code-fg);
}
.row .js {
  font-family: var(--font-mono);
  font-size: 13px;
  color: var(--fg-2);
}
.row .sjs {
  color: var(--fg-1);
}
@media (max-width: 640px) {
  .row {
    grid-template-columns: minmax(0, 1fr);
    gap: 4px;
  }
  .row.head {
    display: none;
  }
}
.more {
  margin: 16px 0 0;
}
.more a {
  color: var(--text-link);
  font-weight: 500;
  text-decoration: none;
}
.ways {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
}
@media (max-width: 900px) {
  .ways {
    grid-template-columns: minmax(0, 1fr);
  }
}
.way {
  display: flex;
  flex-direction: column;
  transition: border-color var(--dur-fast) var(--ease-step);
}
.way:hover {
  border-color: var(--accent);
}
.way .tag {
  align-self: flex-start;
  font: var(--type-label);
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--accent-fg);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-pill);
  padding: 3px 8px;
}
.way p {
  flex: 1;
  margin-bottom: 14px;
}
.way code {
  display: block;
  padding: 8px 10px;
  font: var(--type-code);
  font-size: 12px;
  color: var(--code-fg);
  background: var(--surface-code);
  border: 1px solid var(--code-border);
  border-radius: var(--radius-2);
  overflow-x: auto;
  white-space: nowrap;
}
.pipeline-teaser {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: 48px;
  align-items: center;
}
@media (max-width: 900px) {
  .pipeline-teaser {
    grid-template-columns: minmax(0, 1fr);
  }
}
.mini {
  list-style: none;
  margin: 0;
  padding: 24px;
  display: grid;
  gap: 10px;
  background: var(--bg-grid), var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
}
.mini li {
  display: grid;
  grid-template-columns: 36px 1fr auto;
  align-items: baseline;
  gap: 10px;
  padding: 12px 16px;
  margin: 0;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-left: 3px solid var(--accent);
  border-radius: var(--radius-2);
  font: var(--fw-regular) 20px/1.1 var(--font-serif);
  color: var(--fg-1);
}
.mini li span {
  font: var(--type-label);
  letter-spacing: var(--tracking-label);
  color: var(--accent-fg);
}
.mini small {
  font: var(--type-label);
  color: var(--fg-3);
}
.closing :deep(h2) {
  border: 0;
  padding: 0;
  margin-top: 0;
}
</style>
