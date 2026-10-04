# Modules: imports, const declarations and exports. A module's value is its
# namespace: an object of every export, with keys sorted by UTF-16 code
# units, as JavaScript sorts a namespace's.

=== a module's value is its exports, sorted as a namespace
@module basics
const hidden = 2;
export const double = x => x * hidden;
export const answer = 21 * hidden;
--- value
{"answer":42,"double":<function>}

=== a module that imports another
@module more
import { double, answer as fortyTwo } from "basics";
export const quadruple = x => double(double(x));
export const both = [quadruple(1), fortyTwo];
--- value
{"both":[4,42],"quadruple":<function>}

=== water-filling, written in staticjs
@module power
export const fill = (left, wants) =>
  wants.reduce((acc, w, i) => {
    const kw = Math.min(w.want_kw, acc.left / (wants.length - i));
    return { left: acc.left - kw, out: { ...acc.out, [w.port]: kw } };
  }, { left, out: {} });

export const waterFill = (capacity, demand) => {
  const ports = Object.entries(demand);
  const priorities = [...new Set(ports.map(([, d]) => d.priority))].toSorted((a, b) => b - a);
  return priorities.reduce((acc, prio) => {
    const wants = ports
      .filter(([, d]) => d.priority === prio)
      .map(([port, d]) => ({ port, want_kw: d.want_kw }))
      .toSorted((a, b) => a.want_kw - b.want_kw);
    const r = fill(acc.left, wants);
    return { left: r.left, out: { ...acc.out, ...r.out } };
  }, { left: capacity, out: {} }).out;
};

export const example = waterFill(60, {
  a: { want_kw: 22, priority: 1 },
  b: { want_kw: 7, priority: 1 },
  c: { want_kw: 50, priority: 2 }
});
--- value
{"example":{"c":50,"b":5,"a":5},"fill":<function>,"waterFill":<function>}

=== a module whose functions use imported ones
@module campaign
import { fill } from "power";
export const take = (xs, n) => xs.slice(0, n);
export const drop = (xs, n) => xs.slice(n);
export const without = (ids, id) => ids.difference(new Set([id]));
export const unsent = (batch, pending, sent) => batch.filter(n => pending.has(n.id) && !sent.has(n.id));
export const tracker = id => `#_actor_progress/${id}`;
export const check = fill(10, [{ port: "p", want_kw: 4 }]).out.p;
--- value
{"check":4,"drop":<function>,"take":<function>,"tracker":<function>,"unsent":<function>,"without":<function>}

=== importing a name the module does not export
@module broken
import { nothing } from "basics";
export const x = 1;
--- error name does not export

=== importing from a module that does not exist
@module broken
import { x } from "nowhere";
export const y = 1;
--- error name no module

=== imports may follow declarations
@module late
const a = 1;
import { answer } from "basics";
export const b = a + answer;
--- value
{"b":43}

=== a module cannot use a const before declaring it
@module order
export const a = b;
export const b = 1;
--- error unsupported used before

=== a module-level name read in a function before its declaration
@module order
const f = () => later;
const later = 1;
export const x = f();
--- error unsupported used before

=== a later const cannot change what an earlier function read
@module order
const f = () => String;
const String = 1;
export const x = f();
--- error unsupported used before

=== a name declared twice
@module twice
const a = 1;
const a = 2;
--- error syntax

=== a statement is not a module item
@module statements
if (true) {}
--- error unsupported a staticjs module holds imports, const declarations and exports

=== a destructuring const at module level
@module destructured
const [first, second] = [1, 2];
export const sum = first + second;
--- value
{"sum":3}

# export default

=== a module with a default export
@module dflt
const base = 40;
export const inc = x => x + 1;
export default base + 2;
--- value
{"default":42,"inc":<function>}

=== export default of an object, ended by the source
@module dobj
export default { b: 1, a: [2] }
--- value
{"default":{"b":1,"a":[2]}}

=== export default of an arrow function
@module dfn
export default (a, b) => a + b;
--- value
{"default":<function>}

=== export default of a name bound nowhere
@module e
export default nothing;
--- error name not bound

# Export lists

=== an export list names consts declared after it
@module listed
export { a, b as renamed, c as default };
const a = 1;
const b = 2;
const c = 3;
--- value
{"a":1,"default":3,"renamed":2}

=== one binding exported under two names
@module twonames
export const x = 1;
export { x as y };
--- value
{"x":1,"y":1}

=== exports are sorted by UTF-16 code units, as a namespace is
@module sorted
export const b = 1, B = 2, _ = 3, $ = 4, a1 = 5, a = 6;
export default 7;
export const toString = 8;
--- value
{"$":4,"B":2,"_":3,"a":6,"a1":5,"b":1,"default":7,"toString":8}

=== a reserved word as an exported name
@module words
const a = 1;
export { a as if, a as default };
--- value
{"default":1,"if":1}

=== an empty export list
@module emptylist
export {};
--- value
{}

=== an export list of a name the module does not declare
@module e
export { a };
--- error syntax exported but the module does not declare

=== an export list exports declarations only, not globals or built-ins
@module e
export { String };
--- error syntax exported but the module does not declare

=== export { default } without from
@module e
export { default };
--- error syntax only a re-export

=== export { if } without from
@module e
export { if };
--- error syntax only a re-export

=== a string as a local name without from
@module e
export { "a" as b };
--- error syntax only a re-export

=== a name exported twice in one list
@module e
const a = 1; export { a, a };
--- error syntax exported twice

=== two export default
@module e
export default 1; export default 2;
--- error syntax exported twice

=== export default and a list's default
@module e
export default 1; const a = 2; export { a as default };
--- error syntax exported twice

=== export const and a list export the same name
@module e
export const x = 1; export { x };
--- error syntax exported twice

=== export default takes an expression, not a declaration
@module e
export default const a = 1;
--- error syntax not a declaration

=== export default let
@module e
export default let;
--- error syntax not a declaration

=== export default takes one expression
@module e
export default 1, 2;
--- error syntax one expression

=== export default of a reserved word
@module e
export default default;
--- error syntax expected an expression

=== a module is strict: let is reserved
@module e
export const a = [let];
--- error syntax expected an expression

=== export default needs a value
@module e
export default;
--- error syntax expected an expression

=== export needs a declaration
@module e
export 1;
--- error syntax after export

=== export let is not staticjs
@module e
export let a = 1;
--- error unsupported exports const declarations

=== export function is not staticjs
@module e
export function f() {}
--- error unsupported exports const declarations

=== export class is not staticjs
@module e
export class A {}
--- error unsupported exports const declarations

=== export default function is not staticjs
@module e
export default function () {}
--- error unsupported

=== export default async is not staticjs
@module e
export default async x => x;
--- error unsupported

=== a string as an exported name
@module e
const a = 1; export { a as "x y" };
--- error unsupported not strings

=== a string as an exported name of a name the module does not declare
@module e
export { a as "x y" };
--- error syntax exported but the module does not declare

=== __proto__ cannot be exported
@module e
const a = 1; export { a as __proto__ };
--- error unsupported __proto__

=== export const __proto__ cannot be exported either
@module e
export const __proto__ = 1;
--- error unsupported __proto__

# Imports

=== a default import
@module usesdefault
import answer from "dflt";
export const x = answer;
--- value
{"x":42}

=== a default import with named imports
@module mixed
import answer, { inc } from "dflt";
export const x = inc(answer);
--- value
{"x":43}

=== a default import is the export named default
@module mixed2
import { default as answer } from "dflt";
export const x = answer;
--- value
{"x":42}

=== a default import of a module with no default
@module e
import x from "basics";
export const y = x;
--- error name does not export

=== a namespace import
@module nsuser
import * as ns from "dflt";
export const keys = Object.keys(ns);
export const x = ns.inc(ns.default);
export const missing = ns.nothing;
export const has = "inc" in ns;
--- value
{"has":true,"keys":["default","inc"],"missing":undefined,"x":43}

=== a namespace keeps its order when spread
@module nsspread
import * as s from "sorted";
export const copy = { ...s };
export const names = Object.entries(s).map(([k]) => k).join(" ");
--- value
{"copy":{"$":4,"B":2,"_":3,"a":6,"a1":5,"b":1,"default":7,"toString":8},"names":"$ B _ a a1 b default toString"}

=== a default import with a namespace import
@module both
import answer, * as ns from "dflt";
export const same = answer === ns.default;
--- value
{"same":true}

=== a namespace exported again
@module nsagain
import * as ns from "dflt";
export { ns };
--- value
{"ns":{"default":42,"inc":<function>}}

=== as and from are names
@module contextual
import as, { inc as from } from "dflt";
export const x = from(as);
--- value
{"x":43}

=== imports that bind nothing
@module bare
import "dflt";
import {} from "dflt";
export {} from "dflt";
export const x = 1;
--- value
{"x":1}

=== a namespace has no prototype: reading toString faults
@module e
import * as ns from "dflt";
export const x = ns.toString;
--- fault no prototype

=== a namespace has no prototype: in faults on its names
@module e
import * as ns from "dflt";
export const x = "valueOf" in ns;
--- fault no prototype

=== a namespace is not turned into a string
@module e
import * as ns from "dflt";
export const x = `${ns}`;
--- fault turns only primitives into strings

=== a namespace is not compared by identity
@module e
import * as ns from "dflt";
export const x = ns === ns;
--- fault identity

=== import "m" of a module that does not exist
@module e
import "nowhere";
--- error name no module

=== import {} of a module that does not exist
@module e
import {} from "nowhere";
--- error name no module

=== export {} from a module that does not exist
@module e
export {} from "nowhere";
--- error name no module

=== export * from a module that does not exist
@module e
export * from "nowhere";
--- error name no module

=== a namespace import of a module that does not exist
@module e
import * as ns from "nowhere";
--- error name no module

=== a string as an imported name, with as
@module e
import { "a-b" as x } from "dflt";
--- error unsupported not strings

=== a string as an imported name needs as
@module e
import { "a-b" } from "dflt";
--- error syntax cannot be bound

=== import { default } needs as
@module e
import { default } from "dflt";
--- error syntax cannot be bound

=== import as eval
@module e
import { inc as eval } from "dflt";
--- error syntax expected a name to import as

=== a namespace and a const of one name
@module e
import * as ns from "dflt"; const ns = 1;
--- error syntax declared twice

=== a default import and a named import of one name
@module e
import x, { inc as x } from "dflt";
--- error syntax declared twice

=== a name used before its import
@module e
export const a = answer;
import { answer } from "basics";
--- error unsupported used before

=== a name used in an earlier function before its import
@module e
const f = () => inc(1);
import { inc } from "dflt";
export const x = f();
--- error unsupported used before

=== import inside a function's block
@module e
const f = () => { import { inc } from "dflt"; return 1; };
--- error syntax top level of a module

=== export inside a function's block
@module e
const f = () => { export const a = 1; return 1; };
--- error syntax top level of a module

=== import in an expression
import { a } from "m"
--- error syntax top level of a module

=== import() in a module
@module e
import("dflt");
--- error unsupported import()

=== import.meta in a module
@module e
import.meta;
--- error unsupported import.meta

=== import.meta in an expression
import.meta
--- error syntax top level of a module

=== import() in an expression
import("m")
--- error unsupported import()

=== const is not an expression
[const]
--- error syntax expected an expression

# Re-exports

=== re-exported names
@module re
export { inc, default } from "dflt";
export { inc as plusOne, default as answer } from "dflt";
--- value
{"answer":42,"default":42,"inc":<function>,"plusOne":<function>}

=== a re-export binds no name in the module
@module renobind
export { inc } from "dflt";
const inc = 5;
export const y = inc;
--- value
{"inc":<function>,"y":5}

=== a re-export does not bind its name
@module e
export { inc } from "dflt";
export const y = inc;
--- error name not bound

=== re-exporting a name the module does not export
@module e
export { nothing } from "dflt";
--- error name does not export

=== re-exporting one name twice
@module e
export { inc } from "dflt"; export { inc } from "dflt";
--- error syntax exported twice

=== a string as a re-exported name
@module e
export { "inc" as b } from "dflt";
--- error unsupported not strings

=== export * gives every export but default
@module star
export * from "dflt";
export const own = 1;
--- value
{"inc":<function>,"own":1}

=== export * as a name keeps default
@module starns
export * as lib from "dflt";
--- value
{"lib":{"default":42,"inc":<function>}}

=== export * as default
@module stardefault
export * as default from "dflt";
--- value
{"default":{"default":42,"inc":<function>}}

=== export * as a name and a const of that name
@module e
export * as ns from "dflt"; export const ns = 1;
--- error syntax exported twice

=== export * as a string
@module e
export * as "x" from "dflt";
--- error unsupported not strings

=== a local export hides one from export *
@module shadow
export * from "dflt";
export const inc = "mine";
--- value
{"inc":"mine"}

=== a re-export hides one from export *
@module shadow2
export * from "sorted";
export { inc as a } from "dflt";
--- value
{"$":4,"B":2,"_":3,"a":<function>,"a1":5,"b":1,"toString":8}

=== export * twice from one module
@module startwice
export * from "dflt";
export * from "dflt";
--- value
{"inc":<function>}

=== a module that exports x
@module one
export const x = 1;
--- value
{"x":1}

=== another module that exports x
@module two
export const x = 1;
--- value
{"x":1}

=== two export * that give the same name
@module ambiguous
export * from "one";
export * from "two";
--- error unsupported export *

=== a local export settles two export *
@module settled
export * from "one";
export * from "two";
export const x = 3;
--- value
{"x":3}

=== export * from modules whose names do not meet
@module disjoint
export * from "one";
export * from "dflt";
--- value
{"inc":<function>,"x":1}

=== an imported name exported with export * of its module
@module restar
import { inc } from "dflt";
export * from "dflt";
export { inc };
--- value
{"inc":<function>}

=== export const of several declarators and patterns exports each name
@module declarators
export const [p, q] = [1, 2], { r } = { r: 3 };
--- value
{"p":1,"q":2,"r":3}

=== importing through re-exports
@module through
import { plusOne } from "re";
import * as all from "star";
export const x = plusOne(all.own);
--- value
{"x":2}

=== a re-export list may take from on the next line
@module nextline
export { x }
from "one";
const y = 1;
--- value
{"x":1}

# Automatic semicolon insertion

=== a line break ends a statement
@module asi
const a = 1
export const b = a
export default b
export { a }
import { inc } from "dflt"
export * from "one"
export const f = x => { return inc(x) }
export const g = f(a)
--- value
{"a":1,"b":1,"default":1,"f":<function>,"g":2,"x":1}

=== a line break ends export default after an arrow with a block
@module asidefault
export default x => { return x }
export const y = 1
--- value
{"default":<function>,"y":1}

=== line breaks inside import and export statements
@module asiinside
export
const a = 1
export default
a
import
{ inc }
from
"dflt"
export const x = inc(a)
--- value
{"a":1,"default":1,"x":2}

=== a line break ends a const in a block
@module asiblock
export const f = x => {
  const y = x + 1
  const z = y * 2
  return z
}
export const g = f(1)
--- value
{"f":<function>,"g":4}

=== a line break does not end an expression that goes on
@module asicontinue
export const a = 1
  + 2
export const b = [a]
  .map(x => x * 2)
--- value
{"a":3,"b":[6]}

=== a line break ends a const in an expression's block
(() => {
  const a = 1
  return a + 1
})()
--- value
2

=== two declarations on one line need a semicolon
@module e
export const a = 1 export const b = 2
--- error syntax

=== an import and a declaration on one line need a semicolon
@module e
import { inc } from "dflt" export const x = 1
--- error syntax
