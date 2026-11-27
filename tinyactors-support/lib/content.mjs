// What anyone may read, without waking an actor: the skills (each a
// directory with a SKILL.md, its frontmatter's name and description, and
// the Markdown files beside it, such as references/, by their path) and
// the documentation, Markdown files searched by their words.
//
// The server reads the skills from skills/ and the docs from the
// directories it is given: on the VM, the copy the deploy bundles (the
// guides and READMEs of the packages); here, the checkout's.

import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

function frontmatter(text) {
  const match = /^---\n([\s\S]*?)\n---\n/.exec(text);
  const fields = {};
  for (const line of (match?.[1] ?? '').split('\n')) {
    const at = line.indexOf(':');
    if (at > 0) fields[line.slice(0, at).trim()] = line.slice(at + 1).trim();
  }
  return fields;
}

function markdownFiles(directory) {
  const found = [];
  for (const entry of readdirSync(directory)) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) found.push(...markdownFiles(path));
    else if (entry.endsWith('.md')) found.push(path);
  }
  return found;
}

const words = (text) => String(text).toLowerCase().replace(/[^a-z0-9]+/g, ' ').split(' ').filter((w) => w.length > 1);

export class Content {
  // skills: a directory; docs: [{directory, prefix}], prefix naming them (tinyactors-napi/docs/guide)
  constructor({ skills, docs = [] }) {
    this.skills = existsSync(skills) ? readdirSync(skills).filter((d) => existsSync(join(skills, d, 'SKILL.md'))).sort().map((d) => {
      const text = readFileSync(join(skills, d, 'SKILL.md'), 'utf8');
      const { name = d, description = '' } = frontmatter(text);
      const files = markdownFiles(join(skills, d)).map((path) => relative(join(skills, d), path))
        .filter((path) => path !== 'SKILL.md').sort()
        .map((path) => ({ path, text: readFileSync(join(skills, d, path), 'utf8') }));
      return { name, description, text, files };
    }) : [];
    this.docs = docs.filter((d) => existsSync(d.directory)).flatMap(({ directory, prefix }) => markdownFiles(directory).map((path) => {
      const text = readFileSync(path, 'utf8');
      const title = /^#\s+(.+)$/m.exec(text)?.[1] ?? relative(directory, path);
      return { path: `${prefix}/${relative(directory, path)}`, title, text, words: new Set(words(`${title} ${text}`)) };
    }));
  }

  // The documents holding every word of the query, the most mentions first.
  search(query, limit = 8) {
    const wanted = words(query);
    if (wanted.length === 0) return [];
    return this.docs.filter((d) => wanted.every((w) => d.words.has(w)))
      .map((d) => {
        const lower = d.text.toLowerCase();
        const score = wanted.reduce((n, w) => n + lower.split(w).length - 1, 0) + (wanted.some((w) => d.title.toLowerCase().includes(w)) ? 20 : 0);
        const at = Math.max(0, lower.indexOf(wanted[0]) - 120);
        return { path: d.path, title: d.title, score, excerpt: d.text.slice(at, at + 400).replace(/\s+/g, ' ').trim() };
      })
      .sort((a, b) => b.score - a.score || a.path.localeCompare(b.path)).slice(0, limit)
      .map(({ score: _, ...hit }) => hit);
  }

  doc(path) { return this.docs.find((d) => d.path === path); }
}
