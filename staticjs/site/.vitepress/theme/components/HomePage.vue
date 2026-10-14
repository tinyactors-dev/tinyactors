<script setup lang="ts">
import { withBase } from 'vitepress';
import EngineRace from './EngineRace.vue';
import Icon from './Icon.vue';

// What goes wrong when an application runs code someone else wrote, and what staticjs does about it.
const pains = [
  {
    title: 'A rule that never finishes',
    text: 'A timeout depends on the machine it runs on, and in Node’s vm a promise callback outlives it. staticjs has no loops and no recursion: a program runs on fuel you push, pauses where it runs out, and resumes when you push more.',
  },
  {
    title: 'A way out of the sandbox',
    text: 'A sandbox around a whole engine must take away the network, the files, the globals and the prototypes, and every one it misses is an escape. A staticjs program starts with nothing: it reaches only what you pass in.',
  },
  {
    title: 'Memory that grows until the process dies',
    text: 'Every byte comes from your allocator, every construct has a limit, and nothing recurses, so no program can overflow the C stack. With a small stack, an evaluator holds about 41 KiB.',
  },
  {
    title: 'A different answer on another machine',
    text: 'Time limits depend on the machine, and engines disagree on the last bits of Math. staticjs gives the same value and spends the same fuel everywhere; the clock and Math.random’s seed are yours.',
  },
  {
    title: 'A new language for every author',
    text: 'Rule languages are pure and end, but each has a syntax of its own to learn. staticjs is JavaScript: authors test a rule in a browser console, and their editor already knows it.',
  },
  {
    title: 'A wrong value that looks right',
    text: 'In JavaScript, "5" * 2 is 10. Where JavaScript would coerce, compare identities or look on a prototype, staticjs stops with a fault that says why. Every other value is the one Node gives, and the test suite checks it against Node.',
  },
];

const versus = [
  { what: 'Language', quickjs: 'All of modern JavaScript', sjs: 'Expressions and modules of pure JavaScript: no loops, recursion, classes, async or I/O' },
  { what: 'A program that never ends', quickjs: 'Stopped by an interrupt handler you write', sjs: 'Ends: each evaluation runs on the fuel you push, and pauses where it runs out' },
  { what: 'Memory', quickjs: 'A limit you set on the runtime', sjs: 'Your allocator, and a limit on every construct' },
  { what: '"5" * 2', quickjs: '10, as JavaScript gives', sjs: 'A fault that says why' },
  { what: 'Time, both as WebAssembly', quickjs: 'Measured below, in your browser', sjs: 'Measured below, in your browser' },
  { what: 'Time, staticjs as a C library', quickjs: '4.8 times staticjs’s, over 33 evaluation cases', sjs: 'About 5 times faster than QuickJS-NG in WebAssembly' },
];

const stops = [
  { src: '[1, 2] === [1, 2]', js: 'false', sjs: 'fault: === on two objects compares identity, which staticjs does not have' },
  { src: '"5" * 2', js: '10', sjs: 'fault: arithmetic in staticjs is on numbers' },
  { src: '({}).a.b', js: 'TypeError', sjs: 'fault: cannot read a property of undefined' },
  { src: '() => { for (;;) {} }', js: 'never ends, if called', sjs: 'refused when compiled: staticjs has no loops' },
];

const ways = [
  { title: 'A C library', tag: 'Any C11 compiler', code: 'cc -std=c11 -Iinclude app.c lib/libstaticjs.a', text: 'Static and shared libraries for Linux and macOS, x86_64 and arm64.', link: '/guide/getting-started' },
  { title: 'One C file', tag: 'Built with clang', code: 'clang -O2 -I. app.c staticjs.c', text: 'The amalgamation: the whole library as staticjs.c and staticjs.h.', link: '/guide/getting-started#_3-build-it' },
  { title: 'WebAssembly', tag: 'Browsers and Node', code: "await WebAssembly.instantiate(bytes, {})", text: 'staticjs.wasm imports nothing and brings its own allocator.', link: '/guide/wasm' },
];
</script>

<template>
  <div class="home">
    <section class="hero">
      <div class="copy">
        <h1>Let others write the rules. Every one of them <em>ends</em>.</h1>
        <p class="lede">
          Guards, filters, pricing formulas, computed fields: code someone else writes and your application runs.
          staticjs runs it in a subset of JavaScript that has no loop to hang on, nothing of the host to reach and no
          clock of its own, and gives exactly the value Node gives, or a fault that says why. It is a freestanding C
          library, and a WebAssembly module that imports nothing.
        </p>
        <div class="actions">
          <a class="sjs-button primary big" :href="withBase('/guide/getting-started')">Get started</a>
          <a class="sjs-button big" :href="withBase('/guide/why')">Why staticjs?</a>
        </div>
      </div>
      <figure class="demo">
        <div class="vp-doc"><slot name="demo" /></div>
        <figcaption>Edit it: staticjs.wasm runs it in this page, on the fuel shown.</figcaption>
      </figure>
    </section>

    <section class="band">
      <h2 class="section-title">What running someone else’s code costs you, and what staticjs takes away</h2>
      <p class="lede">
        A sandbox around a JavaScript engine leaves you some of these problems, and a rule language of its own leaves
        you others. staticjs is built so that none of them can happen.
      </p>
      <div class="pains">
        <article v-for="p in pains" :key="p.title" class="card pain">
          <h3>{{ p.title }}</h3>
          <p>{{ p.text }}</p>
        </article>
      </div>
    </section>

    <section class="band">
      <h2 class="section-title">staticjs or QuickJS?</h2>
      <p class="lede">
        QuickJS runs all of JavaScript, and if your users write whole programs, use it. staticjs gives up loops,
        classes and I/O, and in exchange guarantees what a host running someone else’s rules needs.
      </p>
      <table class="compare">
        <thead><tr><th scope="col"></th><th scope="col">QuickJS-NG</th><th scope="col">staticjs</th></tr></thead>
        <tbody>
          <tr v-for="v in versus" :key="v.what">
            <th scope="row">{{ v.what }}</th>
            <td class="other" data-label="QuickJS-NG">{{ v.quickjs }}</td>
            <td data-label="staticjs">{{ v.sjs }}</td>
          </tr>
        </tbody>
      </table>
      <p class="note">
        Measured on one 4-core x86_64 machine, staticjs as a C library and QuickJS-NG compiled to WebAssembly and run
        by Node, on the same programs and inputs: proportions, not a promise. Node with its JIT is faster than both,
        taking 0.43 of staticjs’s time; without its JIT, Node is about as fast as staticjs.
        <a :href="withBase('/guide/performance#compared-with-other-engines')">Every case, and how it was measured <Icon name="arrow-right" :size="14" /></a>
      </p>
    </section>

    <EngineRace />

    <section class="band">
      <h2 class="section-title">A fault, never a plausible wrong value</h2>
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
      <p class="lede">No C library and no imports: one C file builds for Linux, macOS and WebAssembly from the same source.</p>
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
  width: 22%;
  background: none;
  border-bottom: 0;
  border-top: 1px solid var(--border-1);
  vertical-align: baseline;
}
.compare .other {
  color: var(--fg-2);
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
