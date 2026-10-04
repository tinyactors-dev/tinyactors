// Copied from tests/spec/modules.spec, the case "a module whose functions
// use imported ones": it imports "power" (power.js), which bench.c
// evaluates first.
import { fill } from "power";
export const take = (xs, n) => xs.slice(0, n);
export const drop = (xs, n) => xs.slice(n);
export const without = (ids, id) => ids.difference(new Set([id]));
export const unsent = (batch, pending, sent) => batch.filter(n => pending.has(n.id) && !sent.has(n.id));
export const tracker = id => `#_actor_progress/${id}`;
export const check = fill(10, [{ port: "p", want_kw: 4 }]).out.p;
