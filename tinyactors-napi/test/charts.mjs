// Charts the tests share, on the ECMAScript datamodel (a host here holds
// quickjs until the native datamodel is built), and the helpers that make a
// host run them. Actors are observed with what the inputs give: an actor
// that reaches a final state ends, so host.actorByName stops finding it.
import { compileCharts, Host, TinyactorsError } from '@tinyactors/node';

export const QUICKJS = { datamodel: 'quickjs' };
export const CONFIG = { systemID: 't' };

/** An SCXML document `name` whose body is `body`. */
export function scxml(name, body, { datamodel = 'ecmascript', initial = 's' } = {}) {
  return `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="${datamodel}" name="${name}" initial="${initial}">\n${body}\n</scxml>`;
}

/** The image of `charts`, an object of name -> text, read before the
 * compile result is closed. */
export function image(charts) {
  const compiled = compileCharts(Object.entries(charts).map(([name, text]) => ({ name: `${name}.scxml`, text })));
  try {
    return compiled.image;
  } finally {
    compiled.close();
  }
}

/** A quickjs host running `charts`; `host.ref(name)` is a chart's reference. */
export function hostWith(charts, { config = CONFIG, ...options } = {}) {
  const host = new Host(config, { ...QUICKJS, ...options });
  host.addImage(image(charts));
  return host;
}

/** A chart that ends on `event` whose data's `n` is 1. */
export const WAITER = scxml('waiter', `
  <state id="s"><transition event="go" cond="_event.data &amp;&amp; _event.data.n === 1" target="done"/></state>
  <final id="done"/>`);

/** A chart that ends on `event`, whatever its data. */
export const ENDER = scxml('ender', `
  <state id="s"><transition event="go" target="done"/></state>
  <final id="done"/>`);

export function equal(actual, expected, what) {
  if (actual !== expected) throw new Error(`${what}: ${String(actual)}, expected ${String(expected)}`);
}

export function throws(body, check, what) {
  try {
    body();
  } catch (error) {
    check(error);
    return;
  }
  throw new Error(`${what}: not refused`);
}

export const typeError = (error) => {
  if (!(error instanceof TypeError)) throw error;
};

export const status = (name) => (error) => {
  if (!(error instanceof TinyactorsError)) throw error;
  equal(error.status, name, 'status');
};

export const tick = () => new Promise((resolve) => setImmediate(resolve));
export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** Waits, by event-loop turns only, until `done()` holds (at most `limit`
 * turns); the host is never asked to drive. */
export async function until(done, limit = 200) {
  for (let i = 0; i < limit && !done(); i++) await tick();
  return done();
}
