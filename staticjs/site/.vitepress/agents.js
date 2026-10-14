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
export function toAgentMarkdown(source, { evaluate, describeError, title }) {
  const { data, body } = splitFrontmatter(source);
  let text = body
    .replace(/<script setup[\s\S]*?<\/script>\n?/g, '')
    .replace(/<!-- html -->[\s\S]*?<!-- \/html -->\n?/g, '')
    .replace(/<!-- markdown\n([\s\S]*?)-->\n?/g, '$1')
    .replace(/^\s*<[A-Z][A-Za-z]*\b[^>]*\/>\s*$/gm, '')
    .replace(/^\s*<\/?(?:[A-Z][A-Za-z]*|template)\b[^>]*>\s*$/gm, '');
  text = text.replace(/^```([^\n]*)\n([\s\S]*?)^```$/gm, (whole, info, code) => {
    const live = liveInfo(info);
    if (!live) return whole;
    const result = evaluate(code.replace(/\n$/, ''), { module: live.module });
    return `\`\`\`js\n${code}\`\`\`\n\n${outcomeLine(result, describeError)}`;
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
    '> staticjs is a pure, total subset of JavaScript, as a freestanding C library and a WebAssembly module. A program it accepts is valid JavaScript and evaluates to the value JavaScript gives, or to a fault: never a different value. Every evaluation ends (it runs on fuel the host pushes), has no side effects, and allocates only through the host.',
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
    '- [The C header](https://github.com/dhamidi/tinyactors/blob/main/staticjs/include/staticjs.h): the API reference',
    '- [Source](https://github.com/dhamidi/tinyactors/tree/main/staticjs): the library, its tests and design notes', '');
  return lines.join('\n');
}
