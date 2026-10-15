<script setup lang="ts">
import { withBase } from 'vitepress';
import EngineRace from './EngineRace.vue';
import Icon from './Icon.vue';

// The six properties, the first three backing the headline.
const features = [
  { title: 'Small', text: 'One C file, one header, no dependencies, no libc. The language is small as well: no loops, recursion or classes. An evaluator at max_stack = 256 is about 41 KiB.' },
  { title: 'Embeddable', text: 'Freestanding C11. Static and shared libraries for Linux and macOS on x86_64 and arm64, or one wasm module that imports nothing. Runs in browsers and Node.' },
  { title: 'Fast', text: 'A compiled guard evaluates from C in about 0.14 µs. Compiling it takes 6.4 µs. Against QuickJS-NG, both as wasm in Chromium, staticjs won 7 of 8 programs.' },
  { title: 'Total', text: 'Every evaluation ends. The host pushes fuel, one unit per instruction and per element a built-in walks. When it runs out the program pauses, and it resumes from any point once more arrives.' },
  { title: 'Exact', text: 'A program staticjs accepts is valid JavaScript and gives the value Node gives, or a fault that says why. "5" * 2 faults instead of giving 10. About 4,200 test cases also run in Node and must match.' },
  { title: 'Hermetic', text: 'A program sees only the values the host passes in. No file system, network, clock or randomness of its own; the host supplies the clock and Math.random’s seed. Same program and inputs give the same value and the same fuel on every machine.' },
];

// staticjs among QuickJS, CEL and Starlark; "—" where their pages say nothing.
const compare = [
  { what: 'Syntax', sjs: 'JavaScript (a pure subset)', quickjs: 'JavaScript (ES2025)', cel: 'Its own C-like syntax', starlark: 'A dialect of Python' },
  { what: 'Functions users write', sjs: 'Arrow functions, no recursion', quickjs: 'All of JavaScript', cel: 'No; macros and host functions', starlark: 'def functions, no recursion' },
  { what: 'Loops', sjs: 'None; map, filter, reduce, find', quickjs: 'All of JavaScript', cel: 'None; map, filter, all, exists', starlark: 'for over finite collections' },
  { what: 'Runaway programs', sjs: 'Cannot run away; host-pushed fuel', quickjs: 'Host’s interrupt handler stops it', cel: 'Non-Turing complete', starlark: '—' },
  { what: 'Reaches', sjs: 'Only values the host passes in', quickjs: '—', cel: 'Only data the host provides', starlark: 'No file system, network, clock' },
  { what: 'Memory', sjs: 'Host allocator; limit per construct', quickjs: 'Limit set on the runtime', cel: '—', starlark: '—' },
  { what: 'Same result everywhere', sjs: 'Same value and same fuel', quickjs: '—', cel: '—', starlark: 'Deterministic evaluation' },
  { what: 'Coercion', sjs: 'Stops with a fault', quickjs: '—', cel: '—', starlark: '—' },
  { what: 'Embeds in', sjs: 'Freestanding C11, one file; wasm', quickjs: 'A few C files, no dependency', cel: 'Go, Java, C++', starlark: 'Go, Java, Rust' },
];

const others = [
  'QuickJS if you need all of JavaScript. It passes nearly all of the ES2025 tests.',
  'CEL if you need formal verification, or a library for Go, Java or C++.',
  'Starlark if you want Python-style configuration, as Bazel uses it.',
];

const stops = [
  { src: '[1, 2] === [1, 2]', js: 'false', sjs: 'fault: === on two objects compares identity, which staticjs does not have' },
  { src: '"5" * 2', js: '10', sjs: 'fault: arithmetic in staticjs is on numbers' },
  { src: '({}).a.b', js: 'TypeError', sjs: 'fault: cannot read a property of undefined' },
  { src: '() => { for (;;) {} }', js: 'never ends, if called', sjs: 'refused when compiled: staticjs has no loops' },
];

const ways = [
  { title: 'Prebuilt libraries', code: 'cc -std=c11 -Iinclude app.c lib/libstaticjs.a', text: 'Static and shared libraries for Linux and macOS, x86_64 and arm64. Link with any C11 compiler.', link: '/guide/getting-started' },
  { title: 'Amalgamation', code: 'clang -O2 -I. app.c staticjs.c', text: 'staticjs.c and staticjs.h, the whole library. Add both to your project and build with clang.', link: '/guide/getting-started#_3-build-it' },
  { title: 'WebAssembly', code: "await WebAssembly.instantiate(bytes, {})", text: 'staticjs.wasm imports nothing and brings its own allocator. Runs in browsers and Node.', link: '/guide/wasm' },
];
</script>

<template>
  <div class="home">
    <section class="hero">
      <div class="copy">
        <h1>A small, embeddable and fast JavaScript engine</h1>
        <p class="lede">
          A pure subset of JavaScript, for running code your users write. Every program ends, sees only what you hand
          it, and returns what Node returns or a fault with a line and column.
        </p>
        <div class="actions">
          <a class="sjs-button primary big" :href="withBase('/guide/getting-started')">Getting started</a>
          <a class="sjs-button big" href="#compare">Compare with QuickJS, CEL, Starlark</a>
        </div>
        <ul class="facts" aria-label="At a glance">
          <li>One C file, no libc</li>
          <li>Wasm module, no imports</li>
          <li>0.14 µs to evaluate a guard</li>
          <li>Runs on fuel you push</li>
        </ul>
      </div>
      <figure class="demo">
        <div class="vp-doc"><slot name="demo" /></div>
        <figcaption>Edit it. It runs live in staticjs.wasm, and the result shows the fuel spent.</figcaption>
      </figure>
    </section>

    <section class="band">
      <h2 class="section-title">What you get</h2>
      <div class="pains">
        <article v-for="f in features" :key="f.title" class="card pain">
          <h3>{{ f.title }}</h3>
          <p>{{ f.text }}</p>
        </article>
      </div>
    </section>

    <section id="compare" class="band">
      <h2 class="section-title">How staticjs compares</h2>
      <p class="lede">
        Basically CEL with JavaScript syntax and user functions. Next to QuickJS it can’t run away; next to Starlark it’s
        JavaScript instead of a Python dialect.
      </p>
      <table class="compare">
        <thead>
          <tr><th scope="col"></th><th scope="col">staticjs</th><th scope="col">QuickJS</th><th scope="col">CEL</th><th scope="col">Starlark</th></tr>
        </thead>
        <tbody>
          <tr v-for="v in compare" :key="v.what">
            <th scope="row">{{ v.what }}</th>
            <td data-label="staticjs">{{ v.sjs }}</td>
            <td class="other" data-label="QuickJS">{{ v.quickjs }}</td>
            <td class="other" data-label="CEL">{{ v.cel }}</td>
            <td class="other" data-label="Starlark">{{ v.starlark }}</td>
          </tr>
        </tbody>
      </table>
      <ul class="choose">
        <li v-for="o in others" :key="o">{{ o }}</li>
      </ul>
      <p class="note">
        What we say about QuickJS, CEL and Starlark comes from their own pages. The race below runs in your browser; the
        benchmark report has one machine’s numbers.
        <a :href="withBase('/guide/performance#compared-with-other-engines')">The benchmark report <Icon name="arrow-right" :size="14" /></a>
      </p>
    </section>

    <EngineRace />

    <section class="band">
      <h2 class="section-title">When a rule is wrong</h2>
      <p class="lede">Mistakes in a rule come back as a fault with a line and column.</p>
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
      <p class="more"><a :href="withBase('/guide/faults')">All faults and refusals <Icon name="arrow-right" :size="14" /></a></p>
    </section>

    <section class="band">
      <h2 class="section-title">Three ways in</h2>
      <div class="two-up">
        <a v-for="w in ways" :key="w.title" class="card way" :href="withBase(w.link)">
          <h3>{{ w.title }}</h3>
          <p>{{ w.text }}</p>
          <code>{{ w.code }}</code>
        </a>
      </div>
    </section>

    <section class="band pipeline-teaser">
      <div>
        <h2 class="section-title">Inside the pipeline</h2>
        <p class="lede">
          Step through the parser, compiler and evaluator on a program of your own. Push the parser 1, 4 or 16 bytes at a
          time, look at the compiled function shapes, spend fuel one unit at a time, and read the output in 16-byte
          pieces. Runs on staticjs.wasm.
        </p>
        <a class="sjs-button primary big" :href="withBase('/explore/pipeline')">Open the pipeline explorer</a>
      </div>
      <ol class="flow" aria-label="The four stages">
        <li><strong>Parser</strong><span>bytes → events</span></li>
        <li><strong>Compiler</strong><span>events → bytecode</span></li>
        <li><strong>Evaluator</strong><span>bytecode → value</span></li>
        <li><strong>Writer</strong><span>value → JS source</span></li>
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
.facts {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-1) var(--space-5);
  margin: var(--space-5) 0 0;
  padding: 0;
  list-style: none;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.facts li::before {
  content: '●';
  margin-right: var(--space-2);
  color: var(--state-running);
  font-size: 9px;
  vertical-align: 2px;
}
.choose {
  margin: var(--space-5) 0 0;
  padding-left: var(--space-5);
  font: var(--type-body);
  color: var(--fg-1);
}
.choose li + li {
  margin-top: var(--space-1);
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
.pains {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--space-5);
  margin: var(--space-6) 0 0;
}
@media (max-width: 1000px) {
  .pains {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
@media (max-width: 640px) {
  .pains {
    grid-template-columns: minmax(0, 1fr);
  }
}
.pain {
  padding: var(--space-6);
}
.pain h3 {
  font: var(--fw-regular) var(--fs-22) / var(--lh-snug) var(--font-display);
  margin: 0 0 var(--space-3);
  color: var(--fg-1);
}
.pain p {
  margin: 0;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.stops,
.compare {
  width: 100%;
  border-collapse: collapse;
  background: var(--surface-1);
  border: 1px solid var(--border-2);
  font: var(--type-body-sm);
}
.stops th,
.compare th {
  text-align: left;
  font-weight: 600;
  color: var(--fg-1);
  background: var(--surface-2);
  padding: var(--space-3) var(--space-4);
  border-bottom: 1px solid var(--border-2);
}
.stops td,
.compare td {
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
.compare th[scope='row'] {
  width: 16%;
  background: none;
  border-bottom: 0;
  border-top: 1px solid var(--border-1);
  vertical-align: baseline;
}
.compare .other {
  color: var(--fg-2);
}
.compare td:nth-child(2) {
  color: var(--fg-1);
  font-weight: 500;
}
@media (max-width: 720px) {
  .stops thead,
  .compare thead {
    display: none;
  }
  .stops tr,
  .compare tr {
    display: grid;
    gap: 4px;
    padding: var(--space-3) var(--space-4);
    border-top: 1px solid var(--border-1);
  }
  .stops td,
  .compare td,
  .compare th[scope='row'] {
    width: auto;
    padding: 0;
    border: 0;
  }
  .compare td::before {
    content: attr(data-label) ': ';
    font-weight: 600;
    color: var(--fg-1);
  }
}
.note {
  margin: var(--space-4) 0 0;
  max-width: var(--prose-max);
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.more {
  margin: var(--space-4) 0 0;
}
.more a,
.note a {
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
  overflow-wrap: anywhere;
  white-space: pre-wrap;
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
