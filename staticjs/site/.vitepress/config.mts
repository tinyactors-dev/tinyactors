import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitepress';
import { StaticJS, describeError, operator } from './theme/staticjs/runtime.js';
import { liveInfo, llmsTxt, markdownURL, pipelineMarkdown, prefersMarkdown, routeOf, toAgentMarkdown } from './agents.js';
import { umber, vellum } from './shiki-themes.js';

const here = dirname(fileURLToPath(import.meta.url));
const srcDir = resolve(here, '..');
const siteURL = (process.env.STATICJS_SITE_URL ?? '').replace(/\/$/, '');
const repo = 'https://github.com/dhamidi/tinyactors';

// staticjs in Node, for the build: every live sample's result is computed
// here, so the page shows it before any JavaScript runs, and its markdown
// carries it. A sample whose outcome is not what its fence says fails the
// build.
const staticjs = new StaticJS(new WebAssembly.Module(readFileSync(join(srcDir, 'public/staticjs-site.wasm'))));
const evaluate = (source, options) => staticjs.evaluate(source, options);

const guide = [
  { text: 'Getting started', link: '/guide/getting-started', description: 'From a download to a C program that evaluates JavaScript: prebuilt libraries, the amalgamation, a first program.' },
  { text: 'Why staticjs?', link: '/guide/why', description: 'When staticjs fits and when it does not, and how it compares with a full JavaScript engine and with expression languages.' },
  { text: 'Quick reference', link: '/guide/reference', description: 'The language and the C API on one page: the two kinds of program, what to write instead of what staticjs leaves out, the built-ins, the calls in order.' },
  { text: 'Embedding', link: '/guide/embedding', description: 'The C API: contexts, allocators, limits, fuel, values and who owns them, globals, native functions, modules, the clock and the seed, streaming, inspection.' },
  { text: 'The language', link: '/guide/language', description: 'What a staticjs program can say: syntax, modules, every built-in, BigInt, Map, JSON, Temporal, Date, and changing a value a function owns.' },
  { text: 'Faults and refusals', link: '/guide/faults', description: 'Where staticjs stops instead of following JavaScript, and what it refuses when compiled.' },
  { text: 'Performance', link: '/guide/performance', description: 'Fuel, memory, the callbacks that run in C, and a comparison with Node and QuickJS.' },
  { text: 'Browser and Node', link: '/guide/wasm', description: 'staticjs.wasm from JavaScript, in a browser or in Node.' },
  { text: 'Examples', link: '/guide/examples', description: 'Whole C programs and a browser page, built and checked by the test suite.' },
];
const explore = [
  { text: 'The pipeline', link: '/explore/pipeline', description: 'Follow a program through the parser, the compiler, the evaluator and the writer, step by step, live.' },
];

/** The pages, for llms.txt: each with its section, title and description. */
const pages = [
  { route: '/', section: 'Start', title: 'staticjs', description: 'What staticjs is, the language against JavaScript, its properties, how to install it, how it works, its performance, a comparison with QuickJS, CEL and Starlark, and its limitations.' },
  { route: '/guide/', section: 'Guide', title: 'Guide', description: 'Every page of the guide, in reading order.' },
  ...guide.map((p) => ({ route: p.link, section: 'Guide', title: p.text, description: p.description })),
  { route: '/explore/', section: 'Explore', title: 'Explore', description: 'Interactive views of how staticjs works.' },
  ...explore.map((p) => ({ route: p.link, section: 'Explore', title: p.text, description: p.description })),
];

const agentMarkdown = (file) => {
  const source = readFileSync(join(srcDir, file), 'utf8');
  const page = pages.find((p) => p.route === routeOf(file));
  const include = (path) => readFileSync(resolve(srcDir, dirname(file), path), 'utf8');
  const pipeline = (program) => pipelineMarkdown(staticjs.pipeline(program), { operator });
  return toAgentMarkdown(source, { evaluate, describeError, title: page?.title, include, pipeline });
};

export default defineConfig({
  title: 'staticjs',
  description: 'A pure, total subset of JavaScript, as a freestanding C library and a WebAssembly module.',
  lang: 'en',
  cleanUrls: true,
  lastUpdated: false,
  appearance: true,
  srcExclude: ['README.md', 'WRITING.md', 'node_modules/**', 'wasm/**'],
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/favicon.svg' }],
    ['meta', { name: 'theme-color', content: '#F4EEE2' }],
    ['link', { rel: 'alternate', type: 'text/plain', href: '/llms.txt', title: 'llms.txt' }],
  ],
  transformHead({ pageData }) {
    if (pageData.isNotFound || pageData.relativePath === '404.md') return [];
    const route = routeOf(pageData.relativePath);
    return [['link', { rel: 'alternate', type: 'text/markdown', href: markdownURL(route) }]];
  },
  markdown: {
    theme: { light: vellum, dark: umber },
    config(md) {
      const fence = md.renderer.rules.fence;
      md.renderer.rules.fence = (tokens, idx, options, env, self) => {
        const token = tokens[idx];
        const live = liveInfo(token.info);
        if (!live) return fence(tokens, idx, options, env, self);
        const source = token.content.replace(/\n$/, '');
        const result = evaluate(source, { module: live.module });
        if (result.ok === live.fault) {
          const outcome = result.ok ? result.value : describeError(result.error);
          throw new Error(`${env.relativePath}: the live sample ${live.fault ? 'should fault but gives' : 'fails'}: ${outcome}\n${source}`);
        }
        const info = token.info;
        token.info = 'js';
        const code = fence(tokens, idx, options, env, self);
        token.info = info;
        const attr = (value) => encodeURIComponent(value);
        return `<LiveSample source="${attr(source)}" initial="${attr(JSON.stringify(result))}"${live.module ? ' module' : ''}>${code}</LiveSample>\n`;
      };
    },
  },
  themeConfig: {
    logo: { src: '/mark.svg', width: 24, height: 24 },
    nav: [
      { text: 'Guide', link: '/guide/getting-started', activeMatch: '/guide/' },
      { text: 'Pipeline', link: '/explore/pipeline', activeMatch: '/explore/' },
      { text: 'C API', link: `${repo}/blob/main/staticjs/include/staticjs.h` },
      { text: 'llms.txt', link: '/llms.txt', target: '_blank' },
    ],
    sidebar: [
      { text: 'Guide', items: guide.map(({ text, link }) => ({ text, link })) },
      { text: 'Explore', items: explore.map(({ text, link }) => ({ text, link })) },
      { text: 'Reference', items: [
        { text: 'staticjs.h', link: `${repo}/blob/main/staticjs/include/staticjs.h` },
        { text: 'Changelog', link: `${repo}/blob/main/staticjs/CHANGELOG.md` },
        { text: 'Design notes', link: `${repo}/tree/main/staticjs/docs/internals` },
      ] },
    ],
    socialLinks: [{ icon: 'github', link: `${repo}/tree/main/staticjs` }],
    search: { provider: 'local' },
    outline: { level: [2, 3], label: 'On this page' },
    footer: {
      message: 'Every page is also markdown: add <code>.md</code> to its path, or ask for <code>text/markdown</code>. Agents start at <a href="/llms.txt">/llms.txt</a>.',
      copyright:
        '<a class="tinyactors-logo" href="https://github.com/dhamidi/tinyactors" aria-label="staticjs is part of tinyactors">' +
        '<span>staticjs is part of</span>' +
        '<img class="on-light" src="/brand/tinyactors-horizontal-ink.svg" alt="tinyactors" width="112" height="29">' +
        '<img class="on-dark" src="/brand/tinyactors-horizontal-parchment.svg" alt="tinyactors" width="112" height="29">' +
        '</a>',
    },
    docFooter: { prev: 'Previous', next: 'Next' },
  },
  vite: {
    plugins: [
      {
        // The dev server answers markdown as the production server does.
        name: 'staticjs-markdown',
        configureServer(server) {
          server.middlewares.use((req, res, next) => {
            const url = new URL(req.url ?? '/', 'http://localhost');
            const wantsMarkdown = url.pathname.endsWith('.md') || prefersMarkdown(req.headers.accept);
            if (url.pathname === '/llms.txt') {
              res.setHeader('Content-Type', 'text/plain; charset=utf-8');
              res.end(llmsTxt(pages, { siteURL }));
              return;
            }
            if (!wantsMarkdown) return next();
            const route = url.pathname.replace(/\.md$/, '').replace(/\/index$/, '/') || '/';
            const page = pages.find((p) => p.route === route || p.route === `${route}/`);
            if (!page) return next();
            const file = page.route.endsWith('/') ? `${page.route.slice(1)}index.md` : `${page.route.slice(1)}.md`;
            res.setHeader('Content-Type', 'text/markdown; charset=utf-8');
            res.setHeader('Vary', 'Accept');
            res.end(agentMarkdown(file));
          });
        },
      },
    ],
  },
  buildEnd(siteConfig) {
    // Every page as markdown beside its HTML, and the agents' entry points.
    const out = siteConfig.outDir;
    const full = [];
    for (const page of pages) {
      const file = page.route.endsWith('/') ? `${page.route.slice(1)}index.md` : `${page.route.slice(1)}.md`;
      const markdown = agentMarkdown(file);
      const target = join(out, markdownURL(page.route));
      mkdirSync(dirname(target), { recursive: true });
      writeFileSync(target, markdown);
      full.push(`<!-- ${siteURL}${markdownURL(page.route)} -->\n\n${markdown}\nSource: ${siteURL}${markdownURL(page.route)}\n`);
    }
    writeFileSync(join(out, 'llms.txt'), llmsTxt(pages, { siteURL }));
    writeFileSync(join(out, 'llms-full.txt'), full.join('\n- - -\n\n'));
  },
});

