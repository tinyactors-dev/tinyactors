# Modules: named imports, const and export const.

=== a module's value is its exports, in the order declared
@module basics
const hidden = 2;
export const double = x => x * hidden;
export const answer = 21 * hidden;
--- value
{"double":<function>,"answer":42}

=== a module that imports another
@module more
import { double, answer as fortyTwo } from "basics";
export const quadruple = x => double(double(x));
export const both = [quadruple(1), fortyTwo];
--- value
{"quadruple":<function>,"both":[4,42]}

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
{"fill":<function>,"waterFill":<function>,"example":{"c":50,"b":5,"a":5}}

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
{"take":<function>,"drop":<function>,"without":<function>,"unsent":<function>,"tracker":<function>,"check":4}

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

=== imports come first
@module late
const a = 1;
import { answer } from "basics";
export const b = a;
--- error unsupported imports come first

=== a module cannot use a const before declaring it
@module order
export const a = b;
export const b = 1;
--- error name not bound

=== a module-level name read in a function before its declaration
@module order
const f = () => later;
const later = 1;
export const x = f();
--- error name not bound

=== a name declared twice
@module twice
const a = 1;
const a = 2;
--- error syntax

=== export default is not staticjs
@module defaults
export default 1;
--- error unsupported exports const declarations only

=== a statement is not a module item
@module statements
if (true) {}
--- error unsupported a staticjs module holds imports and const declarations

=== a destructuring const at module level
@module destructured
const [first, second] = [1, 2];
export const sum = first + second;
--- value
{"sum":3}
