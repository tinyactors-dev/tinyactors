// The Host and its memory (docs/plan.xml, p1-host): creation from a config
// and options, a refused config as core's error, close twice, the dispose
// symbols, the type tag, and a dropped host whose handlers close over it
// collected with its memory freed.
import { Host, TinyactorsError } from '@tinyactors/node';
import { collect, run, test } from './harness.mjs';

const QUICKJS = { datamodel: 'quickjs' };

function equal(actual, expected, what) {
  if (actual !== expected) throw new Error(`${what}: ${actual}, expected ${expected}`);
}

function throws(body, check, what) {
  try {
    body();
  } catch (error) {
    check(error);
    return;
  }
  throw new Error(`${what}: not refused`);
}

test('a host reads ticksPerSecond from its effective config', () => {
  const host = new Host({ systemID: 't', system: { ticksPerSecond: 1000 } }, QUICKJS);
  equal(host.ticksPerSecond, 1000, 'ticksPerSecond');
  equal(host.closed, false, 'closed');
  host.close();
  const defaults = new Host({ systemID: 't' }, QUICKJS);
  equal(defaults.ticksPerSecond, 1e9, 'the default rate, core\'s');
  defaults.close();
});

test('a refused config throws core\'s error', () => {
  throws(() => new Host({ systemID: 't', x: 1 }, QUICKJS), (error) => {
    if (!(error instanceof TinyactorsError)) throw error;
    equal(error.code, -32602, 'code');
    equal(error.reason, 'unknown', 'reason');
    equal(error.path, 'x', 'path');
    equal(error.status, 'TA_ERROR_INVALID', 'status');
  }, 'an unknown key');
  throws(() => new Host('not a map', QUICKJS), (error) => {
    if (!(error instanceof TinyactorsError)) throw error;
  }, 'a config that is not a map');
});

test('the native datamodel is refused until it is built', () => {
  // p1-datamodel builds it; then a host that names none gets it.
  for (const options of [undefined, {}, { datamodel: 'native' }]) {
    throws(() => new Host({ systemID: 't' }, options), (error) => {
      if (!(error instanceof TinyactorsError)) throw error;
      equal(error.status, 'TA_ERROR_UNSUPPORTED', 'status');
    }, 'a native host');
  }
});

test('options are checked', () => {
  const typeError = (error) => {
    if (!(error instanceof TypeError)) throw error;
  };
  const config = { systemID: 't', ioProcessors: [{ type: 'urn:x' }] };
  throws(() => new Host(config, { ...QUICKJS, bogus: 1 }), typeError, 'an unknown option');
  throws(() => new Host(config, { datamodel: 'v8' }), typeError, 'an unknown datamodel');
  throws(() => new Host(config, { ...QUICKJS, log: 1 }), typeError, 'a handler that is not a function');
  throws(() => new Host(config, { ...QUICKJS, ioProcessors: { 'urn:nope': () => {} } }), typeError,
    'a type the config lacks');
  throws(() => new Host(config, { ...QUICKJS, ioProcessors: { 'urn:x': 1 } }), typeError, 'a processor not a function');
  throws(() => new Host(config, { datamodel: 'native', quickjs: {} }), typeError, 'quickjs limits on a native host');
  throws(() => new Host(config, { ...QUICKJS, quickjs: { maxOps: -1 } }), typeError, 'a negative maxOps');
  throws(() => new Host(config, { ...QUICKJS, quickjs: { realm: 'shared' } }), typeError, 'an unknown realm');
  throws(() => new Host(config, { ...QUICKJS, quickjs: { heap: 1 } }), typeError, 'an unknown quickjs option');
  throws(() => Host(config, QUICKJS), typeError, 'a call without new');
  const host = new Host(config, {
    ...QUICKJS,
    ioProcessors: { 'urn:x': () => {} },
    quickjs: { maxOps: 1000, memoryLimit: 1 << 26, stackLimit: 1 << 20, realm: 'isolated' },
  });
  host.close();
});

test('close twice, and the dispose symbols', async () => {
  const host = new Host({ systemID: 't' }, QUICKJS);
  host.close();
  host.close();
  equal(host.closed, true, 'closed');
  equal(host.ticksPerSecond, 1e9, 'ticksPerSecond after close');
  const disposed = new Host({ systemID: 't' }, QUICKJS);
  disposed[Symbol.dispose]();
  equal(disposed.closed, true, 'closed by Symbol.dispose');
  disposed[Symbol.dispose]();
  const awaited = new Host({ systemID: 't' }, QUICKJS);
  const promise = awaited[Symbol.asyncDispose]();
  if (!(promise instanceof Promise)) throw new Error('asyncDispose gave no promise');
  equal(await promise, undefined, 'asyncDispose\'s value');
  equal(awaited.closed, true, 'closed by Symbol.asyncDispose');
});

test('the handlers are the wrapper\'s, under a symbol, and go at close', () => {
  const log = () => {};
  const host = new Host({ systemID: 't' }, { ...QUICKJS, log });
  equal(Object.keys(host).length, 0, 'enumerable own keys');
  const [key] = Object.getOwnPropertySymbols(host);
  equal(host[key].log, log, 'the log handler');
  host.close();
  equal(host[key], undefined, 'the handlers after close');
});

test('a method on another object is refused by the type tag', () => {
  const close = Host.prototype.close;
  for (const self of [{}, Object.create(Host.prototype), new Error('x')]) {
    throws(() => close.call(self), (error) => {
      if (!(error instanceof TypeError)) throw error;
    }, 'close on another object');
  }
  const getter = Object.getOwnPropertyDescriptor(Host.prototype, 'closed').get;
  throws(() => getter.call({}), (error) => {
    if (!(error instanceof TypeError)) throw error;
  }, 'closed on another object');
});

test('a dropped host whose handlers close over it is collected and frees its memory', async () => {
  await collect();
  const before = process.memoryUsage().external;
  let collected = 0;
  const registry = new FinalizationRegistry(() => collected++);
  let during;
  (() => {
    for (let i = 0; i < 50; i++) {
      const box = {};
      const host = new Host({ systemID: 't' }, {
        ...QUICKJS,
        log: () => box.host, // handler -> box -> host -> handlers -> handler
        ioProcessors: {},
      });
      box.host = host;
      registry.register(host, i);
    }
    during = process.memoryUsage().external;
  })();
  await collect();
  equal(collected, 50, 'hosts collected');
  // The host's blocks are reported with napi_adjust_external_memory. Bun
  // counts them in memoryUsage().external; node 22 does not show them
  // there (its own counter returns to where it was: log entry
  // l-p1-host-node-external), so the memory check is made where it shows.
  if (during - before > 50 * 1024) {
    const after = process.memoryUsage().external;
    if (after - before > (during - before) / 10) {
      throw new Error(`external memory: ${before} before, ${during} with the hosts, ${after} after`);
    }
  }
});

await run();
