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
// Temporal is the vendored temporal-polyfill 1.0.5 (tests/vendor/temporal-polyfill),
// run in a context of its own: Node 22 has no Temporal, and the draft behind
// --harmony-temporal gives wrong values. Every case sees it as the global
// Temporal.
//
// Temporal.Now reads the host clock, which a case fixes with `@clock <epoch
// nanoseconds>` (spec_test sets it with sjs_vm_set_clock). The polyfill's
// Temporal.Now reads Date.now(), whole milliseconds, and the time zone of
// Intl.DateTimeFormat: here the context's Date.now gives the clock's
// milliseconds (floored), and throws in a case without @clock, as staticjs
// faults there; TZ is UTC, the host's time zone in staticjs; and Temporal.Now
// is the polyfill's own, each function called as it is, with the clock's
// nanoseconds past its millisecond added to what it gives (an Instant, or a
// date and time at a fixed offset, which a nanosecond moves as it moves the
// instant).
//
// Date.now() and new Date() read the same clock (docs/date.md): the global
// Date the cases see is Node's, but for those two, which give the clock's
// milliseconds (floored) and throw in a case without @clock.
//
// Math.random draws from a seed, which a case fixes with `@seed <n>`
// (spec_test sets it with sjs_vm_set_seed). V8 seeds Math.random only from
// its --random-seed flag, the first time a context draws, so a seeded case
// runs in a child `node --random-seed=<n>` of its own (this script with
// --seeded <file> <line>), which prints what the case gives; only seeded
// cases spawn one. The flag takes a nonzero int32 (0 is the machine's
// entropy), and a seeded case is an expression, not a module. A case
// without @seed that draws faults in staticjs (Node draws from the
// machine's entropy), and is not compared.
//
// numbers.txt, if given, holds the lines number_test --print, json_test
// --print and regex_test --print write: what staticjs computed from an input
// (a double formatted, a decimal read, a JSON document read and written, a
// regular expression classified or matched, ...), each of which must be
// what Node computes from it; temporal_test --print writes Temporal (and
// Date) expressions and what staticjs gave for each.
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const polyfill = join(dirname(fileURLToPath(import.meta.url)), 'vendor', 'temporal-polyfill', 'global.js');
process.env.TZ = 'UTC';
const temporalContext = vm.createContext({});
vm.runInContext(readFileSync(polyfill, 'utf8'), temporalContext, { filename: polyfill });
globalThis.Temporal = temporalContext.Temporal;
// The clock of the case running: epoch nanoseconds (a BigInt), or null.
let clock = null;
const clockParts = () => {
  if (clock === null) throw new Error('the case reads the clock and has no @clock');
  const milliseconds = clock / 1000000n - (clock % 1000000n < 0n ? 1n : 0n);
  return [Number(milliseconds), clock - milliseconds * 1000000n];
};
vm.runInContext('Date', temporalContext).now = () => clockParts()[0];
const NodeDate = Date;
globalThis.Date = class Date extends NodeDate {
  constructor(...args) {
    if (args.length === 0) super(clockParts()[0]);
    else super(...args);
  }

  static now() {
    return clockParts()[0];
  }
};
{
  const now = Temporal.Now;
  const later = (value) => value.add({ nanoseconds: Number(clockParts()[1]) });
  const clocked = Object.create(null);
  for (const name of Object.getOwnPropertyNames(now)) clocked[name] = now[name];
  clocked.instant = () => later(now.instant());
  clocked.plainDateTimeISO = (...zone) => later(now.plainDateTimeISO(...zone));
  clocked.plainDateISO = (...zone) => later(now.plainDateTimeISO(...zone)).toPlainDate();
  clocked.plainTimeISO = (...zone) => later(now.plainDateTimeISO(...zone)).toPlainTime();
  clocked.zonedDateTimeISO = (...zone) => later(now.zonedDateTimeISO(...zone));
  Object.defineProperty(Temporal, 'Now', { value: clocked });
}
// The Temporal types, each written as its type's from() of its string.
const temporalTypes = ['PlainDate', 'PlainTime', 'PlainDateTime', 'Instant', 'Duration', 'ZonedDateTime',
  'PlainYearMonth', 'PlainMonthDay'];

// What sjs_writer writes for a value.
function show(v) {
  if (v === undefined) return 'undefined';
  if (v === null) return 'null';
  if (typeof v === 'boolean') return String(v);
  if (typeof v === 'number') return Object.is(v, -0) ? '-0' : String(v);
  if (typeof v === 'bigint') return `${v}n`;
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'function') return '<function>';
  // A Date: new Date of its ISO string, or new Date(NaN) when it is invalid.
  if (v instanceof NodeDate) return Number.isNaN(v.getTime()) ? 'new Date(NaN)' : `new Date(${JSON.stringify(v.toISOString())})`;
  // A Temporal value: Temporal.PlainDate.from("2024-01-31"), its type's from()
  // of what its toString() gives with no options.
  const type = temporalTypes.find((t) => v instanceof Temporal[t]);
  if (type) return `Temporal.${type}.from(${JSON.stringify(v.toString())})`;
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
      current = { name: line.slice(4).trim(), line: index + 1, source: [], expected: [], globals: [], module: null, clock: null, seed: null };
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
    if (where === 'source' && current.source.length === 0 && line.startsWith('@clock ')) {
      current.clock = BigInt(line.slice(7).trim());
      return;
    }
    if (where === 'source' && current.source.length === 0 && line.startsWith('@seed ')) {
      current.seed = BigInt(line.slice(6).trim());
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
  clock = c.clock;
  try {
    return await evaluateAt(c, modules);
  } finally {
    clock = null;
  }
}

async function evaluateAt(c, modules) {
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
    if (c.seed !== null) {
      const problem = seededProblem(c, path);
      if (problem) failures.push(`${where}: ${problem}`);
      continue;
    }
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

// A seeded case: its value from a child Node seeded with --random-seed, or
// what is wrong.
function seededProblem(c, path) {
  if (c.module) return 'a seeded module is not compared';
  if (c.seed === 0n || BigInt.asIntN(32, c.seed) !== c.seed) return `@seed ${c.seed}: node --random-seed takes a nonzero int32`;
  const output = execFileSync(process.execPath, [`--random-seed=${c.seed}`, '--no-warnings=ExperimentalWarning',
    '--experimental-vm-modules', fileURLToPath(import.meta.url), '--seeded', path, String(c.line)], { encoding: 'utf8' });
  const got = JSON.parse(output);
  if (got.threw !== undefined) return `JavaScript threw ${got.threw}`;
  return got.value === c.expected ? null : `\n  spec:       ${c.expected}\n  JavaScript: ${got.value}`;
}

if (process.argv[2] === '--seeded') {
  const [path, line] = process.argv.slice(3);
  const c = parseSpec(path).find((x) => x.line === Number(line));
  let got;
  try {
    got = { value: show(await evaluate(c, new Map())) };
  } catch (error) {
    got = { threw: String(error) };
  }
  console.log(JSON.stringify(got));
  process.exit(0);
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
  P(rest) { // <method> <bits> <digits> <text>: toFixed (f), toExponential (e), toPrecision (p)
    const [method, bits, digits, text] = rest.split(' ');
    const name = { f: 'toFixed', e: 'toExponential', p: 'toPrecision' }[method];
    const theirs = toDouble(bits)[name](digits === 'u' ? undefined : Number(digits));
    return text === theirs ? null : `${toDouble(bits)}.${name}(${digits === 'u' ? '' : digits}): staticjs ${text}, JavaScript ${theirs}`;
  },
  Q(rest) { // <radix> <bits> <text>: x.toString(radix)
    const [radix, bits, text] = rest.split(' ');
    const theirs = toDouble(bits).toString(Number(radix));
    return text === theirs ? null : `${toDouble(bits)}.toString(${radix}): staticjs ${text}, JavaScript ${theirs}`;
  },
  W(rest) { // <x> <y> <x ** y>: Math.pow(x, y) and x ** y, bit for bit (a NaN is any NaN)
    const [a, b, result] = rest.split(' ');
    const [x, y] = [toDouble(a), toDouble(b)];
    for (const [name, want] of [['Math.pow', Math.pow(x, y)], ['**', x ** y]]) {
      if (Number.isNaN(want) ? !Number.isNaN(toDouble(result)) : toHex(want) !== result) {
        return `${name} of ${x} and ${y}: staticjs ${result}, JavaScript ${toHex(want)}`;
      }
    }
    return null;
  },
  U(rest) { // <name> <x> <f(x)>: Math[name](x), bit for bit (a NaN is any NaN)
    const [name, a, result] = rest.split(' ');
    const x = toDouble(a);
    const want = Math[name](x);
    if (Number.isNaN(want) ? Number.isNaN(toDouble(result)) : toHex(want) === result) return null;
    return `Math.${name}(${x}): staticjs ${result}, JavaScript ${toHex(want)}`;
  },
  V(rest) { // <y> <x> <atan2(y, x)>: Math.atan2(y, x), bit for bit (a NaN is any NaN)
    const [a, b, result] = rest.split(' ');
    const y = toDouble(a), x = toDouble(b);
    const want = Math.atan2(y, x);
    if (Number.isNaN(want) ? Number.isNaN(toDouble(result)) : toHex(want) === result) return null;
    return `Math.atan2(${y}, ${x}): staticjs ${result}, JavaScript ${toHex(want)}`;
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
  Y(rest) { // <source> <outcome> <deliberate>: a Temporal (or Date) expression, its value or a fault
    const [quoted, ours, deliberate] = rest.split('\t');
    const source = JSON.parse(quoted);
    let want;
    try {
      want = show(new Function(`return (${source}\n);`)());
    } catch (error) {
      want = `!${error}`;
    }
    const fault = ours.startsWith('!');
    temporalOutcomes[fault ? (want.startsWith('!') ? 'thrown' : 'deliberate') : 'value']++;
    if (deliberate === '1' && !fault) return `${source}: staticjs gives ${ours}, where it should fault on purpose`;
    if (want.startsWith('!')) return fault ? null : `${source}: staticjs ${JSON.parse(ours)}, JavaScript throws ${want.slice(1)}`;
    if (fault) return deliberate === '1' ? null : `${source}: staticjs faults (${ours.slice(1)}), JavaScript ${want}`;
    return JSON.parse(ours) === want ? null : `${source}: staticjs ${JSON.parse(ours)}, JavaScript ${want}`;
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
const temporalOutcomes = { value: 0, thrown: 0, deliberate: 0 };
// The reference for Math is V8 compiled without floating-point contraction,
// which is Node on x86_64 (docs/extending.md). Node's build turns
// contraction off only for s390x and ppc64, so on arm64 the C compiler
// fuses multiply-adds in V8's ieee754.cc and some Math results differ in the
// last bit. The fixed Math lists (U, V, W) are compared only on x64; every
// other check runs everywhere.
const mathReference = process.arch === 'x64';
const mathSkipped = new Map();
if (numbers) {
  for (const line of readFileSync(numbers, 'utf8').split('\n')) {
    if (!line) continue;
    const tag = line.slice(0, line.indexOf(' '));
    if (!mathReference && (tag === 'U' || tag === 'V' || tag === 'W')) {
      mathSkipped.set(tag, (mathSkipped.get(tag) ?? 0) + 1);
      continue;
    }
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
  ['P', 'doubles of a fixed list give toFixed, toExponential and toPrecision as JavaScript does'],
  ['Q', 'doubles of a fixed list give toString(radix) in every radix as JavaScript does'],
  ['W', 'pairs of a fixed list give Math.pow(x, y) and x ** y bit for bit'],
  ['U', 'calls on doubles of fixed lists give Math.exp, expm1, log, log1p, log2, log10, sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, asinh, acosh, atanh and cbrt bit for bit'],
  ['V', 'pairs of a fixed list give Math.atan2(y, x) bit for bit'],
  ['J', 'random JSON documents give JSON.stringify(JSON.parse(t))'],
  ['E', `mutated JSON documents read as JSON.parse reads them (${jsonOutcomes.value} read, ` +
    `${jsonOutcomes['!S']} SyntaxErrors where JavaScript throws one, ${jsonOutcomes['!O']} faults of staticjs's own not compared)`],
  ['K', 'JSON.stringify calls, with replacers and spaces, give what JavaScript does'],
  ['X', `random regular expression literals classify as JavaScript reads them (${regexOutcomes.exact} exact, ` +
    `${regexOutcomes.unsupported} unsupported, ${regexOutcomes.syntax} syntax errors), and match on ` +
    `${regexOutcomes.matched} inputs as JavaScript does (${regexOutcomes.lone} faults where it makes a lone surrogate)`],
  ['Y', `Temporal expressions give the polyfill's value or throw where it does (${temporalOutcomes.value} values, ` +
    `${temporalOutcomes.thrown} faults where it throws, ${temporalOutcomes.deliberate} deliberate faults where it reads ` +
    'what the specification refuses)'],
].filter(([tag]) => counts.has(tag)).map(([tag, what]) => `${counts.get(tag)} ${what}`).join(', ');

if (failures.length > 0) {
  console.error(failures.slice(0, 50).join('\n'));
  console.error(`diff-node: ${failures.length} difference(s) from JavaScript`);
  process.exit(1);
}
console.log(`diff-node: ${checked} cases agree with JavaScript (${skipped} faults not compared)` +
  (numberSummary ? `; ${numberSummary}` : ''));
if (mathSkipped.size > 0) {
  const lists = [...mathSkipped].map(([tag, n]) => `${n} ${tag}`).join(', ');
  console.log(`diff-node: skipped on ${process.arch}: the fixed Math lists (${lists}); Node's Math bits there ` +
    'come from fused multiply-adds; reference is x64');
}
