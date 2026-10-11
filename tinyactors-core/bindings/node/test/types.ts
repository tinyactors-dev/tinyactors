// Type-checked with `tsc -p test` (no emit). Exercises the typings of the
// public API, including negative cases via @ts-expect-error.
import {
  scxml, fromSCXML, createSystem, createActor, toPromise, createLink, serveLinks, parseAddress, formatAddress,
  type Snapshot, type Chart, type Actor,
} from '..';

type Door = { opens: number; locked: boolean };
const c = scxml.chart<Door>();
const door: Chart<Door> = c.scxml('door', { initial: 'closed', datamodel: 'ecmascript' },
  c.datamodel({ opens: c.expr(() => 0), locked: c.expr(() => false) }),
  c.state('closed',
    c.transition('open', 'opened', { cond: c.expr(({ locked }) => !locked) },
      c.assign('opens', c.expr(({ opens }) => opens + 1)))),
  c.state('opened',
    c.onentry(c.send('close', { delay: '5s', id: 'autoClose' })),
    c.onexit(c.cancel('autoClose')),
    c.transition('close', 'closed', c.log('closed', c.expr(({ _event, In }) => `${_event.name} ${In('closed')}`)))),
);
// @ts-expect-error not a variable of the chart
c.expr(({ nope }) => nope);
// @ts-expect-error a datamodel entry of the wrong type
c.datamodel({ opens: c.expr(() => 'many') });

const system = createSystem({
  systemID: 'types',
  ecmascript: 'native',
  ioProcessors: { 'urn:x': { location: 'x', send: ({ target, event, data, origin }) => void [target, event, data, origin] } },
  invokers: { 'urn:y': { start: ({ invokeid, params }, { send, done, fail, signal }) => void [invokeid, params, send, done, fail, signal] } },
  hibernation: { store: new Map<string, Uint8Array>() },
});
const actor: Actor<Door> = createActor(door, { system, input: { opens: 1 }, systemID: 'front' }).start();
actor.send({ type: 'open' });
actor.send('close');
// @ts-expect-error an event needs a type
actor.send({ name: 'open' });
const snapshot: Snapshot<Door> = actor.getSnapshot();
const opens: number | undefined = snapshot.context?.opens;
void opens;
actor.subscribe({ next: (s) => void s.value, complete: () => {} });
void toPromise(actor);
// @ts-expect-error createActor needs a system
createActor(door, {});

system.load([fromSCXML('<scxml/>', { documents: { 'a.scxml': '<scxml/>' } })]);
const stats = system.stats();
void stats.charts[0].macrosteps;
const { done } = system.migrate('door', { wake: true });
void done.then((r) => r.migrated + r.failed);
const bytes: Uint8Array = system.snapshot();
const watch: number = system.watchTraces({ charts: ['door'], for: Infinity });
system.unwatchTraces(watch);
createSystem({ restore: bytes, charts: [door] });
const link = createLink(system, { url: 'ws://localhost/link', name: 'server' });
void link.opened.then(() => link.get('lobby').send({ type: 'JOIN' }));
const served = serveLinks(system, { port: 0, exports: { lobby: 'JOIN LEAVE' } });
void served.connections();
const address = parseAddress(formatAddress('types', 1n));
if (address && address.kind === 'session') void (address.session + 1n);
system.stop();
