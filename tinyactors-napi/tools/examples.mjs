// The documentation's examples: every ```js block of README.md and
// docs/guide/*.md, with the ```output block that follows it. Read by
// test/docs.test.mjs, which runs each, and tools/typecheck.mjs, which
// checks each against index.d.ts with tsc.
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

/** The documents with examples, relative to the package's root. */
export function documents(root) {
  return ['README.md', ...readdirSync(join(root, 'docs/guide')).filter((name) => name.endsWith('.md')).sort().map((name) => `docs/guide/${name}`)];
}

/** The fenced blocks of `text`: {info, body, line}, a block's indentation removed. */
export function blocks(text) {
  const found = [];
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const open = /^(\s*)```(\S*)\s*$/.exec(lines[i]);
    if (!open) continue;
    const [, indent, info] = open;
    const body = [];
    let j = i + 1;
    for (; j < lines.length && !/^\s*```\s*$/.test(lines[j]); j++) body.push(lines[j].startsWith(indent) ? lines[j].slice(indent.length) : lines[j]);
    if (j === lines.length) throw new Error(`an unclosed block at line ${i + 1}`);
    found.push({ info, body: body.join('\n'), line: i + 1 });
    i = j;
  }
  return found;
}

/** Each example: its document, line, program and the output it must print (or undefined). */
export function examples(root) {
  const found = [];
  for (const document of documents(root)) {
    const all = blocks(readFileSync(join(root, document), 'utf8'));
    all.forEach((block, i) => {
      if (block.info !== 'js') return;
      const next = all[i + 1];
      found.push({ document, line: block.line, program: block.body, output: next?.info === 'output' ? next.body : undefined });
    });
  }
  return found;
}

/** The file name an example is written to: its document and line. */
export const fileName = (example) => `${example.document.replace(/[/.]/g, '-')}-${example.line}.mjs`;
