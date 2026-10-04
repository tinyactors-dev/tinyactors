// The rule, checked against JavaScript itself: every spec case that
// expects a value must give that value in Node too; every case staticjs
// refuses as a syntax error must be one JavaScript refuses as well; every
// case it refuses as unsupported must be valid JavaScript; and every case it
// refuses for a name (not bound, not exported, no such module) must throw in
// JavaScript too, when it is parsed, linked or evaluated.
//
// A module's value is its namespace (module.namespace), whose keys
// JavaScript sorts as staticjs does. A module registered under a name taken
// before replaces the earlier one, as in spec_test.
//
//   node --experimental-vm-modules tests/diff-node.mjs tests/spec [numbers.txt]
//
// numbers.txt, if given, holds lines of "<hex bits> <text>" that staticjs
// wrote for doubles; each must be String(x) for the same double.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import vm from 'node:vm';

// What sjs_writer writes for a value.
function show(v) {
  if (v === undefined) return 'undefined';
  if (v === null) return 'null';
  if (typeof v === 'boolean') return String(v);
  if (typeof v === 'number') return Object.is(v, -0) ? '-0' : String(v);
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'function') return '<function>';
  if (Array.isArray(v)) return '[' + v.map(show).join(',') + ']';
  if (v instanceof Set) return 'new Set([' + [...v].map(show).join(',') + '])';
  return '{' + Object.keys(v).map((k) => JSON.stringify(k) + ':' + show(v[k])).join(',') + '}';
}

function parseSpec(path) {
  const cases = [];
  let current = null;
  let where = 'outside';
  readFileSync(path, 'utf8').split('\n').forEach((line, index) => {
    if (line.startsWith('=== ')) {
      current = { name: line.slice(4).trim(), line: index + 1, source: [], expected: [], globals: [], module: null };
      cases.push(current);
      where = 'source';
      return;
    }
    if (where !== 'source' && line.startsWith('# ')) return;
    if (where === 'outside') return;
    if (where === 'source' && current.source.length === 0 && line.startsWith('@global ')) {
      const [name, ...rest] = line.slice(8).split(' = ');
      current.globals.push([name, rest.join(' = ')]);
      return;
    }
    if (where === 'source' && current.source.length === 0 && line.startsWith('@module ')) {
      current.module = line.slice(8).trim();
      return;
    }
    if (where === 'source' && line.startsWith('--- ')) {
      const [kind, ...words] = line.slice(4).trim().split(' ');
      current.kind = kind;
      current.code = kind === 'error' ? words.shift() : null;
      where = 'expected';
      return;
    }
    (where === 'source' ? current.source : current.expected).push(line);
  });
  for (const c of cases) {
    c.source = c.source.join('\n').replace(/\s+$/, '');
    c.expected = c.expected.join('\n').replace(/\s+$/, '');
  }
  return cases;
}

const failures = [];
let checked = 0;
let skipped = 0;

function parses(c) {
  try {
    if (c.module) new vm.SourceTextModule(c.source, { identifier: c.module });
    else new Function(`return (${c.source}\n);`);
    return true;
  } catch (error) {
    if (error instanceof SyntaxError) return false;
    throw error;
  }
}

// Evaluate a case: a module (linked against the modules before it) or an
// expression. Answers the module, or the expression's value.
async function evaluate(c, modules) {
  const globals = c.globals.map(([name, expression]) => [name, (0, eval)(`(${expression}\n)`)]);
  if (c.module) {
    const module = new vm.SourceTextModule(c.source, { identifier: c.module });
    await module.link((specifier) => {
      const found = modules.get(specifier);
      if (!found) throw new Error(`no module ${specifier}`);
      return found;
    });
    await module.evaluate();
    return module;
  }
  const names = globals.map(([name]) => name);
  return new Function(...names, `return (${c.source}\n);`)(...globals.map(([, v]) => v));
}

async function runFile(path) {
  const modules = new Map();
  for (const c of parseSpec(path)) {
    const where = `${path}:${c.line}: ${c.name}`;
    if (c.kind === 'error' && c.code === 'name') {
      checked++;
      let threw = false;
      try {
        await evaluate(c, modules);
      } catch {
        threw = true;
      }
      if (!threw) failures.push(`${where}: staticjs says a name is wrong, but JavaScript runs it`);
      continue;
    }
    if (c.kind === 'error') {
      checked++;
      const ok = parses(c);
      if (c.code === 'syntax' && ok) failures.push(`${where}: staticjs says syntax error, but JavaScript reads it`);
      if (c.code === 'unsupported' && !ok) failures.push(`${where}: staticjs says unsupported, but it is not JavaScript`);
      continue;
    }
    if (c.kind !== 'value') {
      skipped++;
      continue;
    }
    checked++;
    try {
      let value = await evaluate(c, modules);
      if (c.module) {
        modules.set(c.module, value);
        value = value.namespace;
      }
      const got = show(value);
      if (got !== c.expected) failures.push(`${where}\n  spec:       ${c.expected}\n  JavaScript: ${got}`);
    } catch (error) {
      failures.push(`${where}: JavaScript threw ${error}`);
    }
  }
}

const [specs, numbers] = process.argv.slice(2);
const files = statSync(specs).isDirectory()
  ? readdirSync(specs).filter((f) => f.endsWith('.spec')).sort().map((f) => join(specs, f))
  : [specs];
for (const file of files) await runFile(file);

let numberCount = 0;
if (numbers) {
  const view = new DataView(new ArrayBuffer(8));
  for (const line of readFileSync(numbers, 'utf8').split('\n')) {
    if (!line) continue;
    const space = line.indexOf(' ');
    view.setBigUint64(0, BigInt('0x' + line.slice(0, space)));
    const x = view.getFloat64(0);
    const ours = line.slice(space + 1);
    const theirs = Object.is(x, -0) ? '-0' : String(x);
    numberCount++;
    if (ours !== theirs) failures.push(`number ${line.slice(0, space)}: staticjs ${ours}, JavaScript ${theirs}`);
  }
}

if (failures.length > 0) {
  console.error(failures.slice(0, 50).join('\n'));
  console.error(`diff-node: ${failures.length} difference(s) from JavaScript`);
  process.exit(1);
}
console.log(`diff-node: ${checked} cases agree with JavaScript (${skipped} faults not compared)` +
  (numbers ? `, and ${numberCount} numbers format as String(x) does` : ''));
