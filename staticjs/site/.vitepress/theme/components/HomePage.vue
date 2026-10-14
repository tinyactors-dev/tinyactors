<script setup lang="ts">
import { withBase } from 'vitepress';
import Icon from './Icon.vue';

const guarantees = [
  { title: 'The value JavaScript gives', text: 'Or a fault that says why. Never a different value: where JavaScript would coerce, compare identities or look on a prototype, staticjs stops.' },
  { title: 'Every evaluation ends', text: 'No loops but the built-ins over collections, no recursion. A program runs on fuel the host pushes, pauses where it runs out, and resumes.' },
  { title: 'No side effects', text: 'A program reaches only what the host passes in. No I/O, no global state, no clock of its own: even Date.now() and Math.random() are the host’s.' },
  { title: 'Deterministic', text: 'The same program on the same inputs gives the same value and the same fuel on every machine, down to the last bit of Math.sin.' },
  { title: 'Bounded memory', text: 'Memory comes from the host’s allocator, every construct has a limit, and nothing recurses, so no program can overflow the C stack.' },
  { title: 'Freestanding', text: 'No C library. One C file builds for Linux, macOS and WebAssembly from the same source, and the wasm module imports nothing.' },
];

const ways = [
  { title: 'A C library', tag: 'Any C11 compiler', code: 'cc -std=c11 -Iinclude app.c lib/libstaticjs.a', text: 'Static and shared libraries for Linux and macOS, x86_64 and arm64.', link: '/guide/getting-started' },
  { title: 'One C file', tag: 'Built with clang', code: 'clang -O2 -I. app.c staticjs.c', text: 'The amalgamation: the whole library as staticjs.c and staticjs.h.', link: '/guide/getting-started#_3-build-it' },
  { title: 'WebAssembly', tag: 'Browsers and Node', code: "await WebAssembly.instantiate(bytes, {})", text: 'staticjs.wasm imports nothing and brings its own allocator.', link: '/guide/wasm' },
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
        <h1>JavaScript that <em>always ends</em>, and never surprises.</h1>
        <p class="lede">
          staticjs is a pure, total subset of JavaScript. It evaluates programs written by someone else (guards, rules,
          filters, computed fields) and gives exactly the value JavaScript would, or a fault that says why. It is a
          freestanding C library, and a WebAssembly module for the browser.
        </p>
        <div class="actions">
          <a class="sjs-button primary big" :href="withBase('/guide/getting-started')">Get started</a>
          <a class="sjs-button big" :href="withBase('/explore/pipeline')">Watch a program run</a>
        </div>
      </div>
      <figure class="demo">
        <div class="vp-doc"><slot name="demo" /></div>
        <figcaption>Edit it: staticjs.wasm runs it in this page, on the fuel shown.</figcaption>
      </figure>
    </section>

    <section class="band">
      <h2 class="section-title">What it guarantees</h2>
      <p class="lede">Six promises, held by the test suite on every change.</p>
      <div class="features">
        <article v-for="g in guarantees" :key="g.title" class="feature">
          <h3>{{ g.title }}</h3>
          <p>{{ g.text }}</p>
        </article>
      </div>
    </section>

    <section class="band">
      <h2 class="section-title">A fault is never a different value</h2>
      <p class="lede">Where JavaScript would coerce, compare identities or look on a prototype, staticjs stops and says why.</p>
      <table class="stops">
        <thead><tr><th scope="col">Program</th><th scope="col">JavaScript</th><th scope="col">staticjs</th></tr></thead>
        <tbody>
          <tr v-for="s in stops" :key="s.src">
            <td><code>{{ s.src }}</code></td>
            <td class="js">{{ s.js }}</td>
            <td><span class="status" data-kind="error">{{ s.sjs }}</span></td>
          </tr>
        </tbody>
      </table>
      <p class="more"><a :href="withBase('/guide/faults')">Every place it stops, and why <Icon name="arrow-right" :size="14" /></a></p>
    </section>

    <section class="band">
      <h2 class="section-title">The same library, wherever it runs</h2>
      <div class="two-up">
        <a v-for="w in ways" :key="w.title" class="card way" :href="withBase(w.link)">
          <h3>{{ w.title }}</h3>
          <p>{{ w.text }} {{ w.tag }}.</p>
          <code>{{ w.code }}</code>
        </a>
      </div>
    </section>

    <section class="band pipeline-teaser">
      <div>
        <h2 class="section-title">Every stage is pushed, none recurses</h2>
        <p class="lede">
          Bytes into the parser, events into the compiler, fuel into the evaluator, buffer space into the writer. Nothing
          blocks or calls back for more input, so the host decides when each step happens.
        </p>
        <a class="sjs-button primary big" :href="withBase('/explore/pipeline')">Open the pipeline explorer</a>
      </div>
      <ol class="flow" aria-label="The four stages">
        <li><strong>Parser</strong><span>bytes → events</span></li>
        <li><strong>Compiler</strong><span>events → program</span></li>
        <li><strong>Evaluator</strong><span>fuel → value</span></li>
        <li><strong>Writer</strong><span>value → text</span></li>
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
  grid-template-columns: minmax(0, 0.95fr) minmax(0, 1fr);
  gap: var(--space-10);
  align-items: center;
  padding: var(--space-16) 0 var(--space-12);
}
@media (max-width: 960px) {
  .hero {
    grid-template-columns: minmax(0, 1fr);
    padding-top: var(--space-10);
    gap: var(--space-8);
  }
}
h1 {
  font: var(--type-display);
  font-size: clamp(42px, 5.6vw, 64px);
  letter-spacing: var(--tracking-display);
  margin: 0 0 var(--space-5);
  color: var(--fg-1);
  text-wrap: balance;
}
h1 em {
  font-style: italic;
  color: var(--accent-fg);
}
.actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-3);
}
.demo {
  min-width: 0;
  margin: 0;
}
.demo figcaption {
  margin-top: -8px;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.band {
  padding: var(--space-12) 0 0;
  border-top: 1px solid var(--border-1);
  margin-top: var(--space-10);
}
.features {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--space-8) var(--space-6);
  margin: var(--space-6) 0 0;
}
@media (max-width: 1000px) {
  .features {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
@media (max-width: 640px) {
  .features {
    grid-template-columns: minmax(0, 1fr);
  }
}
.feature h3 {
  font: var(--type-h3);
  margin: 0 0 var(--space-2);
  color: var(--fg-1);
  padding-top: var(--space-3);
  border-top: 2px solid var(--accent);
  width: fit-content;
}
.feature p {
  margin: 0;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.stops {
  width: 100%;
  border-collapse: collapse;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  font: var(--type-body-sm);
}
.stops th {
  text-align: left;
  font-weight: 600;
  color: var(--fg-1);
  background: var(--surface-2);
  padding: var(--space-3) var(--space-4);
  border-bottom: 1px solid var(--border-2);
}
.stops td {
  padding: var(--space-3) var(--space-4);
  border-top: 1px solid var(--border-1);
  vertical-align: baseline;
}
.stops code {
  font: var(--type-code);
  color: var(--code-fg);
  white-space: nowrap;
}
.stops .js {
  font-family: var(--font-mono);
  font-size: 13px;
  color: var(--fg-2);
}
.stops .status {
  font: var(--type-body-sm);
  color: var(--fg-1);
}
@media (max-width: 720px) {
  .stops thead {
    display: none;
  }
  .stops tr {
    display: grid;
    gap: 4px;
    padding: var(--space-3) var(--space-4);
    border-top: 1px solid var(--border-1);
  }
  .stops td {
    padding: 0;
    border: 0;
  }
}
.more {
  margin: var(--space-4) 0 0;
}
.more a {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--text-link);
  font-weight: 500;
  text-decoration: none;
}
.two-up {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
  gap: var(--space-5);
  margin: var(--space-6) 0 0;
}
.way {
  display: flex;
  flex-direction: column;
  padding: var(--space-6);
  color: inherit;
  text-decoration: none;
  transition: border-color var(--dur-fast) var(--ease-step);
}
.way:hover {
  border-color: var(--accent);
}
.way h3 {
  font: var(--fw-regular) var(--fs-22) / var(--lh-snug) var(--font-display);
  margin: 0 0 var(--space-3);
  color: var(--fg-1);
}
.way p {
  flex: 1;
  margin: 0 0 var(--space-4);
  color: var(--fg-2);
  font: var(--type-body-sm);
}
.way code {
  display: block;
  padding: var(--space-2) var(--space-3);
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
  gap: var(--space-10);
  align-items: center;
}
@media (max-width: 900px) {
  .pipeline-teaser {
    grid-template-columns: minmax(0, 1fr);
  }
}
.flow {
  list-style: none;
  margin: 0;
  padding: var(--space-6);
  display: grid;
  gap: var(--space-6);
  background: var(--bg-grid), var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
  counter-reset: none;
}
.flow li {
  position: relative;
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: var(--space-4);
  padding: var(--space-3) var(--space-4);
  margin: 0;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  border-radius: var(--radius-2);
}
.flow li + li::before {
  content: '';
  position: absolute;
  left: 50%;
  top: calc(-1 * var(--space-6));
  height: var(--space-6);
  border-left: 1px solid var(--border-strong);
}
.flow strong {
  font: var(--fw-regular) 20px/1.1 var(--font-display);
  color: var(--fg-1);
}
.flow span {
  font: var(--type-code);
  font-size: 12.5px;
  color: var(--fg-2);
}
</style>
