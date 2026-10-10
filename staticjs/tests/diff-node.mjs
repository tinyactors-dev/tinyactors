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
// numbers.txt, if given, holds the lines number_test --print, json_test
// --print and regex_test --print write: what staticjs computed from an input
// (a double formatted, a decimal read, a JSON document read and written, a
// regular expression classified or matched, ...), each of which must be
// what Node computes from it.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import vm from 'node:vm';

// What sjs_writer writes for a value.
function show(v) {
  if (v === undefined) return 'undefined';
  if (v === null) return 'null';
  if (typeof v === 'boolean') return String(v);
  if (typeof v === 'number') return Object.is(v, -0) ? '-0' : String(v);
  if (typeof v === 'bigint') return `${v}n`;
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'function') return '<function>';
  if (v instanceof RegExp) return String(v);
  if (Array.isArray(v)) {
    // A match result: an array with properties of its own besides its elements.
    const extra = Object.keys(v).filter((k) => !/^(0|[1-9][0-9]*)$/.test(k));
    const items = '[' + v.map(show).join(',') + ']';
    if (extra.length === 0) return items;
    return 'Object.assign(' + items + ',{' + extra.map((k) => JSON.stringify(k) + ':' + show(v[k])).join(',') + '})';
  }
  if (v instanceof Set) return 'new Set([' + [...v].map(show).join(',') + '])';
  if (v instanceof Map) return 'new Map([' + [...v].map(([k, x]) => '[' + show(k) + ',' + show(x) + ']').join(',') + '])';
  if (v instanceof Iterator) return '[' + [...v].map(show).join(',') + '].values()';
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
    if (where === 'source' && current.source.length === 0 && line.startsWith('@shapes ')) {
      return; // which functions staticjs tags: nothing to compare with Node
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

// Lines of number_test --print: a tag, then what staticjs computed and
// what from. Each must be what Node computes from the same input.
const counts = new Map();
const view = new DataView(new ArrayBuffer(8));
const toDouble = (hex) => {
  view.setBigUint64(0, BigInt('0x' + hex));
  return view.getFloat64(0);
};
const hexBigInt = (t) => (t.startsWith('-') ? -BigInt('0x' + t.slice(1)) : BigInt('0x' + t));
const toHex = (x) => {
  view.setFloat64(0, x);
  return view.getBigUint64(0).toString(16).padStart(16, '0');
};
const sameDouble = (hex, x) => Object.is(toDouble(hex), x);
const numberChecks = {
  F(rest) { // <bits> <text>: String(x)
    const space = rest.indexOf(' ');
    const x = toDouble(rest.slice(0, space));
    const theirs = Object.is(x, -0) ? '-0' : String(x);
    return rest.slice(space + 1) === theirs ? null : `String: staticjs ${rest.slice(space + 1)}, JavaScript ${theirs}`;
  },
  D(rest) { // <bits> <decimal>: Number(decimal)
    const space = rest.indexOf(' ');
    const want = Number(rest.slice(space + 1));
    return sameDouble(rest.slice(0, space), want) ? null : `Number: staticjs ${rest.slice(0, space)}, JavaScript ${toHex(want)}`;
  },
  N(rest) { // <bits> <bits> <text>: Number(text), parseFloat(text)
    const [number, float] = rest.split(' ', 2);
    const text = rest.slice(number.length + float.length + 2);
    if (!sameDouble(number, Number(text))) return `Number: staticjs ${number}, JavaScript ${toHex(Number(text))}`;
    if (!sameDouble(float, parseFloat(text))) return `parseFloat: staticjs ${float}, JavaScript ${toHex(parseFloat(text))}`;
    return null;
  },
  I(rest) { // <radix> <bits> <text>: parseInt(text, radix)
    const [radix, bits] = rest.split(' ', 2);
    const want = parseInt(rest.slice(radix.length + bits.length + 2), Number(radix));
    return sameDouble(bits, want) ? null : `parseInt: staticjs ${bits}, JavaScript ${toHex(want)}`;
  },
  S(rest) { // <x> <sqrt> <fround>
    const [x, root, round] = rest.split(' ');
    const v = toDouble(x);
    if (!sameDouble(root, Math.sqrt(v))) return `Math.sqrt: staticjs ${root}, JavaScript ${toHex(Math.sqrt(v))}`;
    if (!sameDouble(round, Math.fround(v))) return `Math.fround: staticjs ${round}, JavaScript ${toHex(Math.fround(v))}`;
    return null;
  },
  H(rest) { // <hypot> <x>...
    const [result, ...args] = rest.split(' ');
    const want = Math.hypot(...args.map(toDouble));
    return sameDouble(result, want) ? null : `Math.hypot: staticjs ${result}, JavaScript ${toHex(want)}`;
  },
  C(rest) { // <a> <b> <clz32(a)> <imul(a, b)>
    const [a, b, clz, imul] = rest.split(' ');
    const [x, y] = [toDouble(a), toDouble(b)];
    if (Number(clz) !== Math.clz32(x)) return `Math.clz32: staticjs ${clz}, JavaScript ${Math.clz32(x)}`;
    if (Number(imul) !== Math.imul(x, y)) return `Math.imul: staticjs ${imul}, JavaScript ${Math.imul(x, y)}`;
    return null;
  },
  B(rest) { // <op> <a> <b> <result>, BigInts in hex
    const [op, a, b, result] = rest.split(' ');
    const [x, y] = [hexBigInt(a), hexBigInt(b)];
    let want;
    try {
      want = op === '+' ? x + y : op === '-' ? x - y : op === '*' ? x * y : op === '/' ? x / y : op === '%' ? x % y
        : op === '^' ? x ** y : String(x < y ? -1 : x > y ? 1 : 0);
    } catch {
      want = '!';
    }
    if (op === '<') return result === want ? null : `BigInt ${a} < ${b}: staticjs ${result}, JavaScript ${want}`;
    if (want === '!') return result === '!' ? null : `BigInt ${a} ${op} ${b}: staticjs ${result}, JavaScript throws`;
    return result !== '!' && hexBigInt(result) === want ? null : `BigInt ${a} ${op} ${b}: staticjs ${result}, JavaScript ${want.toString(16)}`;
  },
  T(rest) { // <a> <radix> <decimal> <text>
    const [a, radix, decimal, text] = rest.split(' ');
    const x = hexBigInt(a);
    if (String(x) !== decimal) return `String(${a}): staticjs ${decimal}, JavaScript ${x}`;
    if (BigInt(decimal) !== x) return `BigInt(${decimal}) is not ${a}`;
    const theirs = x.toString(Number(radix));
    return text === theirs ? null : `${a}.toString(${radix}): staticjs ${text}, JavaScript ${theirs}`;
  },
  M(rest) { // <a> <bits> <c>: a compared with a double
    const [a, bits, c] = rest.split(' ');
    const [x, d] = [hexBigInt(a), toDouble(bits)];
    const want = x < d ? -1 : x > d ? 1 : x <= d ? 0 : 2;
    return Number(c) === want ? null : `BigInt ${a} against ${d}: staticjs ${c}, JavaScript ${want}`;
  },
  R(rest) { // <a> <bits>: Number(a)
    const [a, bits] = rest.split(' ');
    const want = Number(hexBigInt(a));
    return sameDouble(bits, want) ? null : `Number(${a}): staticjs ${bits}, JavaScript ${toHex(want)}`;
  },
  A(rest) { // <bits> <a> <asIntN> <asUintN>
    const [bits, a, signed, unsigned] = rest.split(' ');
    const x = hexBigInt(a);
    const [i, u] = [BigInt.asIntN(Number(bits), x), BigInt.asUintN(Number(bits), x)];
    if (hexBigInt(signed) !== i) return `BigInt.asIntN(${bits}, ${a}): staticjs ${signed}, JavaScript ${i.toString(16)}`;
    return hexBigInt(unsigned) === u ? null : `BigInt.asUintN(${bits}, ${a}): staticjs ${unsigned}, JavaScript ${u.toString(16)}`;
  },
  J(rest) { // <document> <text>: JSON.stringify(JSON.parse(document)), never a fault
    const [document, ours] = rest.split('\t');
    const want = JSON.stringify(JSON.parse(JSON.parse(document)));
    if (ours.startsWith('!')) return `JSON round trip: staticjs faults (${ours}), JavaScript ${want}`;
    return JSON.parse(ours) === want ? null : `JSON round trip: staticjs ${ours}, JavaScript ${JSON.stringify(want)}`;
  },
  E(rest) { // <document> <text>: the same, or a SyntaxError where JavaScript throws one
    const [document, ours] = rest.split('\t');
    let want;
    try {
      want = JSON.stringify(JSON.parse(JSON.parse(document)));
    } catch (error) {
      want = error instanceof SyntaxError ? '!S' : `!${error}`;
    }
    jsonOutcomes[ours.startsWith('!') ? ours : 'value']++;
    if (ours === '!O') return null; // a fault of staticjs's own (a lone surrogate, __proto__): not compared
    if (ours === '!S') return want === '!S' ? null : `JSON.parse: staticjs says SyntaxError, JavaScript ${want}`;
    if (want.startsWith('!')) return `JSON.parse: staticjs ${ours}, JavaScript throws ${want}`;
    return JSON.parse(ours) === want ? null : `JSON.parse: staticjs ${ours}, JavaScript ${JSON.stringify(want)}`;
  },
  X(rest) { // <pattern> <flags> <input> <outcome>: the literal /pattern/flags
    const [quoted, flags, input, outcome] = rest.split('\t');
    const pattern = JSON.parse(quoted);
    let re;
    try {
      re = new Function(`return /${pattern}/${flags};`)();
    } catch (error) {
      if (!(error instanceof SyntaxError)) return `/${pattern}/${flags}: Node throws ${error}`;
      regexOutcomes.syntax++;
      return outcome === 'syntax' ? null : `/${pattern}/${flags}: staticjs says ${outcome}, JavaScript refuses it (${error.message})`;
    }
    if (outcome === 'syntax') return `/${pattern}/${flags}: staticjs says syntax error, but JavaScript reads it`;
    if (input !== '-') {
      // The methods on an input, as regex_test's program calls them.
      const s = JSON.parse(input);
      const f = flags.replace(/[gy]/g, '');
      const want = new Function('s', `return JSON.stringify([s.search(/${pattern}/${f}), ` +
        `s.replace(/${pattern}/${f}, "<$&|$1|$2>"), s.split(/${pattern}/${f}), s.match(/${pattern}/${f}g), ` +
        `s.replace(/${pattern}/${f}g, "<$&>"), /${pattern}/${f}.test(s), ` +
        `(m => m && [m.index, [...m], m.groups, m.indices])(/${pattern}/${f}.exec(s)), ` +
        `[...s.matchAll(/${pattern}/${f}g)].map(m => [m.index, ...m]), ` +
        `s.replace(/${pattern}/${f}g, (...a) => JSON.stringify(a))]);`)(s);
      regexOutcomes.matched++;
      if (outcome.startsWith('!')) {
        // A fault where JavaScript makes a string with a lone surrogate.
        regexOutcomes.lone++;
        return /\\ud[89a-f][0-9a-f]{2}/.test(want) ? null : `/${pattern}/${flags} on ${input}: staticjs faults (${outcome}), JavaScript ${want}`;
      }
      return JSON.parse(outcome) === want ? null : `/${pattern}/${flags} on ${input}: staticjs ${JSON.parse(outcome)}, JavaScript ${want}`;
    }
    regexOutcomes[outcome] = (regexOutcomes[outcome] ?? 0) + 1;
    if (outcome === 'exact' || outcome === 'unsupported' || outcome === 'limit') return null;
    return `/${pattern}/${flags}: an unknown outcome ${outcome}`;
  },
  K(rest) { // <source> <text>: JSON.stringify(value, replacer, space), never a fault
    const [source, ours] = rest.split('\t');
    const want = new Function(`return (${JSON.parse(source)}\n);`)();
    if (ours.startsWith('!')) return `JSON.stringify: staticjs faults (${ours}), JavaScript ${JSON.stringify(want)}`;
    const got = ours === 'u' ? undefined : JSON.parse(ours);
    return got === want ? null : `JSON.stringify: staticjs ${ours}, JavaScript ${JSON.stringify(want)}`;
  },
};
const jsonOutcomes = { value: 0, '!S': 0, '!O': 0 };
const regexOutcomes = { exact: 0, unsupported: 0, syntax: 0, matched: 0, lone: 0 };
if (numbers) {
  for (const line of readFileSync(numbers, 'utf8').split('\n')) {
    if (!line) continue;
    const tag = line.slice(0, line.indexOf(' '));
    const check = numberChecks[tag];
    const problem = check ? check(line.slice(tag.length + 1)) : 'an unknown tag';
    counts.set(tag, (counts.get(tag) ?? 0) + 1);
    if (problem) failures.push(`number_test ${line.slice(0, 200)}: ${problem}`);
  }
}
const numberSummary = [
  ['F', 'numbers format as String(x) does'],
  ['D', 'decimals read as Number(s) does'],
  ['N', 'strings read as Number(s) and parseFloat(s) do'],
  ['I', 'strings read as parseInt(s, radix) does'],
  ['S', 'doubles give Math.sqrt(x) and Math.fround(x)'],
  ['H', 'argument lists give Math.hypot(...xs)'],
  ['C', 'pairs give Math.clz32(a) and Math.imul(a, b)'],
  ['B', 'BigInt operations (+ - * / % ** <) give what JavaScript does'],
  ['T', 'BigInts write as String(x) and toString(radix) do, and read back'],
  ['M', 'BigInts compare with doubles as JavaScript does'],
  ['R', 'BigInts round to Number(x)'],
  ['A', 'BigInts wrap as BigInt.asIntN and asUintN do'],
  ['J', 'random JSON documents give JSON.stringify(JSON.parse(t))'],
  ['E', `mutated JSON documents read as JSON.parse reads them (${jsonOutcomes.value} read, ` +
    `${jsonOutcomes['!S']} SyntaxErrors where JavaScript throws one, ${jsonOutcomes['!O']} faults of staticjs's own not compared)`],
  ['K', 'JSON.stringify calls, with replacers and spaces, give what JavaScript does'],
  ['X', `random regular expression literals classify as JavaScript reads them (${regexOutcomes.exact} exact, ` +
    `${regexOutcomes.unsupported} unsupported, ${regexOutcomes.syntax} syntax errors), and match on ` +
    `${regexOutcomes.matched} inputs as JavaScript does (${regexOutcomes.lone} faults where it makes a lone surrogate)`],
].filter(([tag]) => counts.has(tag)).map(([tag, what]) => `${counts.get(tag)} ${what}`).join(', ');

if (failures.length > 0) {
  console.error(failures.slice(0, 50).join('\n'));
  console.error(`diff-node: ${failures.length} difference(s) from JavaScript`);
  process.exit(1);
}
console.log(`diff-node: ${checked} cases agree with JavaScript (${skipped} faults not compared)` +
  (numberSummary ? `; ${numberSummary}` : ''));
