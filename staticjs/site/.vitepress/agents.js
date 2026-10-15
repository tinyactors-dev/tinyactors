// Markdown for agents: every page of the site is also served as markdown,
// at its route plus ".md" and to a request whose Accept header prefers
// text/markdown, and /llms.txt lists them. The build (config.mts,
// buildEnd), the dev server (config.mts, a Vite middleware) and the
// production server (server.mjs) share these functions.

/** A page's source split into its frontmatter (flat `key: value` lines) and its body. */
export function splitFrontmatter(source) {
  const match = /^---\n([\s\S]*?)\n---\n?/.exec(source);
  if (!match) return { data: {}, body: source };
  const data = {};
  for (const line of match[1].split('\n')) {
    const m = /^([A-Za-z]+):\s*(.*)$/.exec(line);
    if (m) data[m[1]] = m[2].replace(/^(['"])(.*)\1$/, '$2');
  }
  return { data, body: source.slice(match[0].length) };
}

/** A live sample's fence info: `js live`, `js live module`, `js live fault`. */
export function liveInfo(info) {
  const m = /^(js|javascript)\s+live\b(.*)$/.exec(info.trim());
  if (!m) return null;
  return { module: /\bmodule\b/.test(m[2]), fault: /\bfault\b/.test(m[2]) };
}

/** One line for a live sample's outcome, as the markdown gives it. */
export function outcomeLine(result, describeError) {
  if (result.ok) return `Result: \`${result.value}\` (${result.fuel} units of fuel)`;
  return describeError(result.error);
}

/**
 * The page as markdown for an agent: no frontmatter, no Vue; a live sample
 * becomes its code and the result the build computed; a block meant only for
 * the browser (`<!-- html -->` ... `<!-- /html -->`) goes, and a block meant
 * only for markdown (`<!-- markdown` ... `-->`) is unwrapped; links to
 * other pages point at their markdown.
 */
/**
 * A program's way through the four stages, as markdown: what the explorer
 * shows, for one program, from `StaticJS.pipeline`.
 */
export function pipelineMarkdown(r, { operator }) {
  const payload = (ev) => {
    if (ev.kind === 'NUMBER') return String(ev.number);
    if (['UNARY', 'BINARY', 'LOGICAL', 'LOGICAL_END'].includes(ev.kind)) return operator(ev.count);
    if (ev.text) return ev.kind === 'STRING' ? JSON.stringify(ev.text) : ev.text;
    if (['CALL_END', 'NEW_END', 'ARROW_BEGIN', 'TEMPLATE_END'].includes(ev.kind)) return String(ev.count);
    return '';
  };
  const deepest = r.steps.reduce((best, s, i) => (s.frames.length > r.steps[best].frames.length ? i : best), 0);
  const frames = r.steps[deepest].frames.map((f, i) =>
    `${i + 1}. ${f.kind === 'loop' ? `\`${f.object ? `${f.object}.` : ''}${f.name}\`${f.length ? `, element ${f.index} of ${f.length}` : ''}` : i === 0 ? 'the program' : 'a function'}, at ${f.at.line}:${f.at.column}`);
  return [
    '## A worked example',
    '',
    '```js',
    r.source,
    '```',
    '',
    `**Parser.** ${r.bytes} bytes become ${r.events.length} events. The first ${Math.min(12, r.events.length)}:`,
    '',
    '| # | Event | Payload | At |',
    '|---:|---|---|---|',
    ...r.events.slice(0, 12).map((ev, i) => `| ${i} | \`${ev.kind}\` | ${payload(ev) ? `\`${payload(ev)}\`` : ''} | ${ev.at.line}:${ev.at.column} |`),
    '',
    `**Compiler.** A program of ${r.compile.programBytes} bytes with ${r.functions.length} functions:`,
    '',
    ...r.functions.map((f, i) => `- ${i === 0 ? 'the program' : `function ${i}`}, at ${f.at.line}:${f.at.column}: ${f.shape ? `shape \`${f.shape}\`` : 'no shape'}`),
    '',
    r.fuelWithoutShapes ? `It runs on ${r.fuel} units of fuel with its shapes, and ${r.fuelWithoutShapes} without them, to the same value.` : '',
    '',
    `**Evaluator.** At step ${deepest} of ${r.steps.length - 1}, its deepest, the frames are, outermost first:`,
    '',
    ...frames,
    '',
    `**Writer.** Through a ${r.pieceSize}-byte buffer, in ${r.output.pieces} pieces, it writes:`,
    '',
    '```js',
    r.output.text,
    '```',
  ].join('\n');
}

export function toAgentMarkdown(source, { evaluate, describeError, title, include, pipeline }) {
  const { data, body } = splitFrontmatter(source);
  let text = body
    // A snippet included from a file (`<<< path{lang}`): its contents, fenced.
    .replace(/^<<< (\S+?)(?:\{(\w+)\})?$/gm, (whole, path, lang) => {
      const code = include(path).replace(/\n$/, '');
      return `\`\`\`${lang ?? path.split('.').pop()}\n${code}\n\`\`\``;
    })
    // A collapsed block: its title as a paragraph, its contents shown.
    .replace(/^::: details (.*)$/gm, '$1:')
    .replace(/^:::$/gm, '')
    // The pipeline explorer's worked example, computed by the build.
    .replace(/^<!-- pipeline example: (.*) -->$/gm, (whole, json) => pipeline(JSON.parse(json)))
    .replace(/<script setup[\s\S]*?<\/script>\n?/g, '')
    .replace(/<!-- html -->[\s\S]*?<!-- \/html -->\n?/g, '')
    .replace(/<!-- markdown\n([\s\S]*?)-->\n?/g, '$1')
    .replace(/^\s*<[A-Z][A-Za-z]*\b[^>]*\/>\s*$/gm, '')
    .replace(/^\s*<\/?(?:[A-Z][A-Za-z]*|template)\b[^>]*>\s*$/gm, '');
  text = text.replace(/^```([^\n]*)\n([\s\S]*?)^```$/gm, (whole, info, code) => {
    const live = liveInfo(info);
    if (!live) return whole;
    const result = evaluate(code.replace(/\n$/, ''), { module: live.module });
    const kind = live.module ? '// A module: its value is the object of its exports.\n' : '';
    return `\`\`\`js\n${kind}${code}\`\`\`\n\n${outcomeLine(result, describeError)}`;
  });
  // Links to the site's pages, absolute or relative, without an extension: their markdown.
  text = text.replace(/\]\((\/(?!\/)[^)#\s]*?)(#[^)\s]*)?\)/g, (whole, path, hash = '') =>
    /\.[a-z0-9]+$/i.test(path) ? whole : `](${path === '/' ? '/index' : path.replace(/\/$/, '/index')}.md${hash})`);
  text = text.replace(/\n{3,}/g, '\n\n').trim();
  if (!/^# /m.test(text)) text = `# ${data.title ?? title ?? 'staticjs'}\n\n${text}`;
  return `${text}\n`;
}

/** The route of a page's source file: "guide/language.md" → "/guide/language", "index.md" → "/". */
export function routeOf(file) {
  const route = `/${file.replace(/\.md$/, '')}`.replace(/\/index$/, '/');
  return route === '' ? '/' : route;
}

/** The markdown URL of a route: "/" → "/index.md", "/guide/x" → "/guide/x.md". */
export function markdownURL(route) {
  return route === '/' ? '/index.md' : route.endsWith('/') ? `${route}index.md` : `${route}.md`;
}

/** Whether an Accept header prefers markdown to HTML, by its q-values. */
export function prefersMarkdown(accept) {
  if (!accept) return false;
  let markdown = 0;
  let html = 0;
  for (const part of accept.split(',')) {
    const [type, ...params] = part.trim().toLowerCase().split(';').map((s) => s.trim());
    const q = Number((params.find((p) => p.startsWith('q=')) ?? 'q=1').slice(2));
    if (type === 'text/markdown' || type === 'text/x-markdown') markdown = Math.max(markdown, q);
    if (type === 'text/html' || type === 'application/xhtml+xml') html = Math.max(html, q);
  }
  return markdown > 0 && markdown >= html;
}

/** /llms.txt: the site for an agent, as llmstxt.org lays it out. */
export function llmsTxt(pages, { siteURL = '' } = {}) {
  const sections = new Map();
  for (const page of pages) {
    if (!sections.has(page.section)) sections.set(page.section, []);
    sections.get(page.section).push(page);
  }
  const lines = [
    '# staticjs',
    '',
    '> staticjs is a small, embeddable and fast JavaScript engine for a pure, total subset of the language, as a freestanding C library and a WebAssembly module. A program it accepts is valid JavaScript and evaluates to the value JavaScript gives, or to a fault: never a different value. Every evaluation ends (it runs on fuel the host pushes), has no side effects, and allocates only through the host.',
    '',
    'Like JavaScript, the language has arrow functions, destructuring, Map, Set, BigInt, JSON and Temporal. Unlike JavaScript, it has no loops, recursion, classes, exceptions, async or I/O, and every evaluation ends. Programs written for Node usually need rewriting with map, filter and reduce. The Quick reference lists what to write instead.',
    '',
    'Every page of this site is markdown at its path plus `.md`, and at its path for a request whose `Accept` header prefers `text/markdown`. `llms-full.txt` holds them all in one file. The C API reference is the public header, `staticjs.h`, whose every declaration has a comment.',
    '',
  ];
  for (const [section, list] of sections) {
    lines.push(`## ${section}`, '');
    for (const page of list) lines.push(`- [${page.title}](${siteURL}${markdownURL(page.route)})${page.description ? `: ${page.description}` : ''}`);
    lines.push('');
  }
  lines.push('## Optional', '', `- [Everything in one file](${siteURL}/llms-full.txt): every page above, in order`,
    '- [The C header](https://raw.githubusercontent.com/tinyactors-dev/tinyactors/main/staticjs/include/staticjs.h): the API reference, as plain text',
    '- [Source](https://github.com/tinyactors-dev/tinyactors/tree/main/staticjs): the library, its tests and design notes', '');
  return lines.join('\n');
}
