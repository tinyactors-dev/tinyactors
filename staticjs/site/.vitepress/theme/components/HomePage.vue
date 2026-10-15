<script setup lang="ts">
import { withBase } from 'vitepress';
import ArchDiagram from './ds/ArchDiagram.vue';
import MemoryLayout from './ds/MemoryLayout.vue';
import Sheet from './ds/Sheet.vue';
import EngineRace from './EngineRace.vue';
import EvaluationFrames from './EvaluationFrames.vue';
import type { Block, Wire, Zone } from './ds/arch';
import Icon from './Icon.vue';

const properties = [
  { name: 'Total', text: 'Every evaluation ends: the evaluator runs on fuel the host pushes, and when the fuel runs out it pauses and resumes from where it stopped, inside a callback included.' },
  { name: 'Exact', text: 'A program staticjs accepts is valid JavaScript. Where JavaScript would coerce, compare objects by identity or look on a prototype, it faults with a line, a column and a fixed message.' },
  { name: 'Hermetic', text: 'A program reads only the globals the host passes in by name, calls only the native C functions it adds and imports only the modules it provides. It has no file system, network, clock or randomness of its own.' },
  { name: 'Deterministic', text: 'The same program with the same inputs gives the same value and uses the same fuel on every machine.' },
  { name: 'Bounded', text: 'Every byte comes from the allocator the host gives, and every construct has a limit set by the host.' },
];

const faults = [
  '[1, 2] === [1, 2]     // fault: === on two objects compares identity, which staticjs does not have',
  '"5" * 2               // fault: arithmetic in staticjs is on numbers',
  '({}).a.b              // fault: cannot read a property of undefined',
  '() => { for (;;) {} } // refused when compiled: staticjs has no loops',
].join('\n');

const ways = [
  { title: 'Prebuilt libraries', code: 'cc -std=c11 -Iinclude app.c lib/libstaticjs.a', text: 'Static and shared libraries for Linux and macOS on x86_64 and arm64. Link with any C11 compiler.', link: '/guide/getting-started' },
  { title: 'Amalgamation', code: 'clang -O2 -I. app.c staticjs.c', text: 'staticjs.c and staticjs.h, two files to add to a C project. They build with clang.', link: '/guide/getting-started#_3-build-it' },
  { title: 'npm', code: 'npm install @tinyactors/staticjs', text: 'For Node and browsers: staticjs as WebAssembly, with JavaScript values in and out.', link: '/guide/wasm' },
];

const compare = [
  { what: 'Syntax', sjs: 'Subset of JavaScript', quickjs: 'All of JavaScript (ES2025)', cel: 'C-like syntax of its own', starlark: 'Dialect of Python' },
  { what: 'Functions in programs', sjs: 'Arrow functions', quickjs: 'All JavaScript functions', cel: 'None; the host provides them', starlark: 'def functions' },
  { what: 'Loops', sjs: 'None', quickjs: 'All JavaScript loops', cel: 'None; macros such as all, map, filter', starlark: 'for over finite sequences' },
  { what: 'Every evaluation ends', sjs: 'Yes; fuel the host pushes', quickjs: 'No; the host’s interrupt handler stops it', cel: 'Yes; not Turing complete', starlark: 'Yes; no recursion, no unbounded loops' },
  { what: 'What a program can reach', sjs: 'Values and modules the host passes in', quickjs: '—', cel: 'Variables and functions the host provides', starlark: 'Hermetic' },
  { what: 'Determinism', sjs: 'Same value and fuel on every machine', quickjs: '—', cel: '—', starlark: 'Deterministic' },
  { what: 'Memory limits', sjs: 'Host’s allocator; limits set by the host', quickjs: 'A memory limit on the runtime', cel: '—', starlark: '—' },
  { what: 'Coercion and identity', sjs: 'Faults on both', quickjs: 'Follows JavaScript: "5" * 2 is 10', cel: '—', starlark: '—' },
  { what: 'Implementations', sjs: 'Freestanding C11, WebAssembly', quickjs: 'A few C files; QuickJS-NG fork', cel: 'Go, Java, C++', starlark: 'Go, Java, Rust' },
];

const others = [
  'CEL also has type checking and formal verification.',
  'Starlark also runs threads in parallel over frozen shared data.',
]

const limits = [
  'A program written for Node usually needs rewriting with map, filter and reduce. What the overview lists as left out is refused when the program is compiled, as an unsupported error.',
  'Valid JavaScript that coerces or compares objects by identity faults instead of running, as the examples in the overview show.',
  'Temporal supports the ISO calendar with UTC and fixed offsets. IANA time zones are not available. Date is UTC only.',
  'Regular expressions run in linear time, without backtracking. Lookahead, lookbehind, backreferences, \\p{...} and the v flag are not supported.',
  'The WebAssembly module (about 950 KiB) is larger than QuickJS’s (about 620 KiB).',
  'staticjs is at version 0.9.0, and the API may change.',
]

// The four stages and the host, as the design system draws architecture.
const pipeline: { blocks: Block[]; zones: Zone[]; wires: Wire[] } = {
  zones: [{ x: 200, y: 16, w: 794, h: 298, label: 'staticjs', meta: 'memory from the host’s allocator' }],
  blocks: [
    { id: 'host', kind: 'external', name: 'host', tech: 'C or JavaScript', x: 16, y: 104, w: 150, h: 120,
      ports: [{ id: 'src', side: 'right', at: 0.25 }, { id: 'fuel', side: 'top', at: 0.5 }, { id: 'value', side: 'right', at: 0.8 },
        { id: 'text', side: 'bottom', at: 0.5 }] },
    { id: 'parser', kind: 'component', name: 'parser', tech: 'bytes, in any chunks', x: 232, y: 96, w: 160, h: 72,
      ports: [{ id: 'in', side: 'left' }, { id: 'out', side: 'right' }] },
    { id: 'compiler', kind: 'component', name: 'compiler', tech: 'events → bytecode', x: 432, y: 96, w: 160, h: 72,
      ports: [{ id: 'in', side: 'left' }, { id: 'out', side: 'right' }] },
    { id: 'program', kind: 'memory', name: 'program', tech: 'bytecode, shapes', x: 632, y: 96, w: 140, h: 72,
      ports: [{ id: 'in', side: 'left' }, { id: 'out', side: 'right' }] },
    { id: 'evaluator', kind: 'component', name: 'evaluator', tech: 'runs on fuel', x: 812, y: 96, w: 160, h: 72,
      ports: [{ id: 'in', side: 'left' }, { id: 'fuel', side: 'top' }, { id: 'out', side: 'bottom' }] },
    { id: 'result', kind: 'memory', name: 'result', tech: 'sjs_value', x: 812, y: 222, w: 160, h: 72,
      ports: [{ id: 'in', side: 'top' }, { id: 'read', side: 'top', at: 0.15 }, { id: 'write', side: 'left' }] },
    { id: 'writer', kind: 'component', name: 'writer', tech: 'value → source text', x: 612, y: 222, w: 160, h: 72,
      ports: [{ id: 'in', side: 'right' }, { id: 'out', side: 'left' }] },
  ],
  wires: [
    { from: 'host.src', to: 'parser.in', kind: 'stream', label: 'bytes' },
    { from: 'parser.out', to: 'compiler.in', kind: 'stream', label: 'events' },
    { from: 'compiler.out', to: 'program.in', kind: 'call', label: 'finish' },
    { from: 'program.out', to: 'evaluator.in', kind: 'shared', label: 'read' },
    { from: 'host.fuel', to: 'evaluator.fuel', kind: 'call', label: 'fuel', hot: true, mid: 68 },
    { from: 'evaluator.out', to: 'result.in', kind: 'call', label: 'take' },
    { from: 'result.read', to: 'host.value', kind: 'shared', label: 'read through the value API', via: [[836, 200]] },
    { from: 'result.write', to: 'writer.in', kind: 'call' },
    { from: 'writer.out', to: 'host.text', kind: 'stream', label: 'text, in pieces' },
  ],
};

// sjs_value, as staticjs.h declares it.
const valueFields = [
  { name: 'kind', type: 'uint8_t', size: 1, note: 'sjs_kind' },
  { name: 'internal', type: 'uint8_t', size: 1, note: 'zero outside the library' },
  { name: 'reserved', type: 'uint16_t', size: 2 },
  { name: 'boolean', type: 'uint32_t', size: 4, note: 'SJS_BOOLEAN' },
  { name: 'as', type: 'double | sjs_heap *', size: 8, kind: 'ptr' as const, note: 'a number, or a heap object for strings and the kinds after them' },
];

const header = 'https://github.com/dhamidi/tinyactors/blob/main/staticjs/include/staticjs.h';
const docs = [
  { label: 'Guide (Getting started)', href: withBase('/guide/getting-started') },
  { label: 'The language', href: withBase('/guide/language') },
  { label: 'Faults and refusals', href: withBase('/guide/faults') },
  { label: 'Embedding', href: withBase('/guide/embedding') },
  { label: 'staticjs.h', href: header },
  { label: 'Pipeline explorer', href: withBase('/explore/pipeline') },
];
</script>

<template>
  <div class="home">
    <section class="hero">
      <div class="copy">
        <h1>A small, embeddable and fast JavaScript engine</h1>
        <p class="lede">
          staticjs is a JavaScript engine for a pure, total subset of the language: every program it accepts ends, and
          gives the value Node gives or a fault.
        </p>
        <div class="actions">
          <a class="sjs-button primary big" :href="withBase('/guide/getting-started')">Getting started</a>
          <a class="sjs-button big" href="#install">Install</a>
        </div>
      </div>
      <figure class="demo">
        <div class="vp-doc"><slot name="demo" /></div>
        <figcaption>Edit the program to filter and sort orders; the result and fuel used update.</figcaption>
      </figure>
    </section>

    <section class="band overview">
      <h2 class="section-title">Overview</h2>
      <div class="prose">
        <p>
          Like JavaScript, staticjs has numbers, BigInt, strings, template literals, arrays, objects, Set, Map, Temporal
          and Date, arrow functions, destructuring with defaults and rest, spread, optional chaining and the usual
          operators. A program is one expression, or a module of imports, const declarations and exports. Unlike
          JavaScript, it has no loops and no recursion.
        </p>
        <p>
          staticjs leaves out loops (for, while, do), recursion, classes, async/await and promises, generators,
          try/catch/throw, var, labels, switch, eval, this, prototypes, getters and setters, and I/O. A function may
          change only a value it owns, such as an array it made; shared values never change.
        </p>
      </div>
      <dl class="properties">
        <div v-for="p in properties" :key="p.name">
          <dt>{{ p.name }}</dt>
          <dd>{{ p.text }}</dd>
        </div>
      </dl>
      <p class="code-intro">What staticjs gives for four valid JavaScript programs:</p>
      <pre class="faults"><code>{{ faults }}</code></pre>
    </section>

    <section id="install" class="band">
      <h2 class="section-title">Install</h2>
      <p class="lede">
        staticjs is freestanding C11: it needs no C library, only stdint.h, stddef.h and stdbool.h, and has one public
        header, staticjs.h.
      </p>
      <div class="two-up">
        <a v-for="w in ways" :key="w.title" class="card way" :href="withBase(w.link)">
          <h3>{{ w.title }}</h3>
          <p>{{ w.text }}</p>
          <code>{{ w.code }}</code>
        </a>
      </div>
    </section>

    <section class="band">
      <h2 class="section-title">Performance</h2>
      <p class="prose-p">
        On a 4-core x86_64 machine, evaluating a compiled small expression from C takes about 0.14 µs, and compiling it
        takes 6.4 µs. Node’s vm.Script.runInContext takes 1.4 µs for the same expression, and creating a fresh V8
        context takes 330 µs. Over 33 evaluation cases, Node with its JIT takes 0.43 of staticjs’s time, so Node’s JIT is
        faster. Node with --jitless takes about as long as staticjs, and QuickJS-NG compiled to WebAssembly takes 4.8
        times as long.
        <a :href="withBase('/guide/performance#compared-with-other-engines')">Benchmark report <Icon name="arrow-right" :size="14" /></a>
      </p>
      <EngineRace />
    </section>

    <section class="band">
      <h2 class="section-title">Limitations</h2>
      <ul class="limits">
        <li v-for="l in limits" :key="l">{{ l }}</li>
      </ul>
    </section>

    <section class="band">
      <h2 class="section-title">Comparison</h2>
      <p class="lede">staticjs is basically CEL with JavaScript syntax and user functions.</p>
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
    </section>

    <section class="band how">
      <h2 class="section-title">How it works</h2>
      <Sheet grid class="pipeline-sheet">
        <ArchDiagram :blocks="pipeline.blocks" :zones="pipeline.zones" :wires="pipeline.wires" :width="1010" :height="330" :grid="false" legend />
      </Sheet>
      <div class="prose">
        <p>
          staticjs has four stages: the parser, the compiler, the evaluator and the writer. A caller pushes into each
          one: bytes into the parser, which sends events to a sink, operands before their operator; the compiler is
          that sink and makes bytecode as the events arrive; fuel into the evaluator; buffer space into the writer.
          None blocks or calls back for more input, so a host can stream source in chunks, and there is no recursion
          anywhere in the C code. The host takes the evaluator’s value and reads it in place, kind by kind and element
          by element, or has the writer write it as JavaScript source.
        </p>
        <p>
          The compiler also tags functions whose shape a built-in can run in C, such as <code>x =&gt; x.key</code> or
          <code>(a, b) =&gt; a.k - b.k</code>. A tagged function saves fuel and never changes a value. A host can pause
          an evaluation and read its frames, for example the program, map at element 2 of 4, and the callback, each
          with a position.
        </p>
      </div>

      <h3 class="minor-title">Frames of an evaluation</h3>
      <p class="prose-p">
        The program below runs in this page one unit of fuel at a time, and after each unit the page reads the
        evaluation’s frames. The comparator <code>(a, b) =&gt; a - b</code> has a shape that <code>toSorted</code> runs in C,
        so it opens no frame of its own.
      </p>
      <EvaluationFrames source="[[3, 1], [2]].map((row) => row.toSorted((a, b) => a - b).map((n) => n * 10))" />
      <p class="prose-p explorer">
        The explorer runs staticjs.wasm in the browser and shows the parser’s events, the compiler’s functions and shapes
        with the fuel used with and without them, the evaluator one unit of fuel at a time with its frames, and the
        result read through the value API and written in 16-byte pieces.
      </p>
      <a class="sjs-button primary big" :href="withBase('/explore/pipeline')">Open the explorer</a>

      <h3 class="minor-title">Values</h3>
      <p class="prose-p">
        Every value is a 16-byte <code>sjs_value</code>: its kind, a boolean, and a number or a pointer to a
        reference-counted heap object. Copying the struct does not retain the object.
      </p>
      <MemoryLayout name="sjs_value" :fields="valueFields" :bytes-per-row="8" :row-height="48" />
    </section>

    <section class="band docs-band">
      <h2 class="section-title">Documentation</h2>
      <ul class="docs">
        <li v-for="d in docs" :key="d.label"><a :href="d.href">{{ d.label }} <Icon name="arrow-right" :size="14" /></a></li>
      </ul>
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
.how .pipeline-sheet {
  margin: 0 0 var(--space-6);
}
.how .pipeline-sheet :deep(.ta-sheet__body) {
  padding: var(--space-4) var(--space-4) var(--space-3);
}
.how .pipeline-sheet :deep(.ta-arch) {
  width: max-content;
  max-width: 100%;
  margin: 0 auto;
}
.how .minor-title {
  margin-top: var(--space-10);
}
.how .explorer {
  margin-top: var(--space-6);
}
.how :deep(.ta-mem) {
  max-width: 640px;
}
.prose p,
.prose-p {
  max-width: var(--prose-max);
  margin: 0 0 var(--space-4);
  font: var(--type-body);
  line-height: 1.65;
  color: var(--fg-1);
}
.prose code {
  font: var(--type-code);
  font-size: 0.9em;
  color: var(--code-fg);
}
.prose-p a,
.docs a {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  color: var(--text-link);
  font-weight: 500;
  text-decoration: none;
}
.properties {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: var(--space-4) var(--space-8);
  margin: var(--space-6) 0;
}
.properties dt {
  font: var(--type-h3);
  color: var(--fg-1);
}
.properties dd {
  margin: var(--space-1) 0 0;
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.code-intro {
  max-width: var(--prose-max);
  margin: var(--space-6) 0 var(--space-3);
  font: var(--type-body);
  color: var(--fg-1);
}
.faults {
  margin: 0;
  padding: var(--space-4) var(--space-5);
  font: var(--type-code);
  font-size: 13px;
  color: var(--code-fg);
  background: var(--surface-code);
  border: 1px solid var(--code-border);
  border-radius: var(--radius-2);
  overflow-x: auto;
}
.limits {
  max-width: var(--prose-max);
  margin: 0 0 var(--space-8);
  padding-left: var(--space-5);
  font: var(--type-body);
  color: var(--fg-1);
}
.limits li + li {
  margin-top: var(--space-2);
}
.minor-title {
  font: var(--type-h3);
  color: var(--fg-1);
  margin: 0 0 var(--space-3);
}
.docs {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2) var(--space-6);
  margin: 0;
  padding: 0;
  list-style: none;
  font: var(--type-ui);
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
