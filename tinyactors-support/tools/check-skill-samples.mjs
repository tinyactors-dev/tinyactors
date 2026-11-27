// Runs the code samples of the skills (skills/*/SKILL.md and references/),
// so a sample that stops working fails the test.
//
//   node tools/check-skill-samples.mjs [--python] [skill-dir...]
//
// In each Markdown file, fenced blocks are read in order, into a scratch
// directory of that file's own:
//   ```xml <path>      written to <path> (a chart a later sample compiles)
//   ```py <path>       written to <path> (a Python file a later sample reads)
//   ```js [<name>]     written to <name> (else sample-<n>.mjs) and run with node
//   ```python [<name>] run with python, with --python only (PYTHON names the
//                      interpreter, PYTHONPATH the built extension)
//   ```output          what the sample before it printed, exactly
// Any other block (sh, text, plain xml) is prose.

import { execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, symlinkSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const python = args.includes('--python');
const dirs = args.filter((a) => !a.startsWith('--'));
const skills = dirs.length ? dirs.map((d) => resolve(d)) : readdirSync(join(root, 'skills')).map((d) => join(root, 'skills', d));

function blocks(markdown) {
  const found = [];
  const re = /^```([^\n]*)\n([\s\S]*?)^```$/gm;
  for (let m; (m = re.exec(markdown)) !== null;) {
    const [kind = '', name = ''] = m[1].trim().split(/\s+/);
    found.push({ kind, name, text: m[2], line: markdown.slice(0, m.index).split('\n').length });
  }
  return found;
}

function markdownFiles(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? markdownFiles(join(dir, e.name)) : e.name.endsWith('.md') ? [join(dir, e.name)] : []);
}

let ran = 0, skipped = 0;
const failures = [];
for (const skill of skills) {
  for (const file of markdownFiles(skill)) {
    const scratch = mkdtempSync(join(tmpdir(), 'skill-samples-'));
    symlinkSync(join(root, 'node_modules'), join(scratch, 'node_modules'));
    let last = null, n = 0;
    try {
      for (const block of blocks(readFileSync(file, 'utf8'))) {
        const where = `${relative(root, file)}:${block.line}`;
        if ((block.kind === 'xml' || block.kind === 'py') && block.name) {
          mkdirSync(dirname(join(scratch, block.name)), { recursive: true });
          writeFileSync(join(scratch, block.name), block.text);
        } else if (block.kind === 'js' || block.kind === 'python') {
          last = null;
          if (block.kind === 'python' && !python) { skipped++; continue; }
          const name = block.name || `sample-${++n}.${block.kind === 'js' ? 'mjs' : 'py'}`;
          writeFileSync(join(scratch, name), block.text);
          const command = block.kind === 'js' ? process.execPath : (process.env.PYTHON ?? 'python3');
          try {
            last = { where, stdout: execFileSync(command, [name], { cwd: scratch, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], timeout: 30_000 }) };
            ran++;
          } catch (error) {
            failures.push(`${where}: exited ${error.status}\n${error.stderr || error.message}`);
          }
        } else if (block.kind === 'output' && last) {
          if (last.stdout.trimEnd() !== block.text.trimEnd())
            failures.push(`${last.where}: printed\n${last.stdout}\nnot\n${block.text}`);
          last = null;
        }
      }
    } finally {
      rmSync(scratch, { recursive: true, force: true });
    }
  }
}
console.log(`${ran} samples ran${skipped ? `, ${skipped} Python samples skipped (--python runs them)` : ''}`);
if (failures.length) {
  console.error(failures.join('\n\n'));
  process.exit(1);
}
