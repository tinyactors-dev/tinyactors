'use strict';

// Run from a fresh project that installed the packed tarballs (mise run
// node:smoke, node:smoke-containers): @tinyactors/node must load its addon
// from the platform package, not from a build tree, and run a chart under
// both "ecmascript" datamodels.

const assert = require('node:assert/strict');
const path = require('node:path');
const { scxml, createSystem, createActor } = require('@tinyactors/node');

const { version } = require('@tinyactors/node/package.json');
const platform = `@tinyactors/node-${process.platform}-${process.arch}`;
const main = path.dirname(require.resolve('@tinyactors/node/package.json'));
const addon = require.resolve(platform, { paths: [main] });
assert.equal(require(`${platform}/package.json`).version, version, 'the platform package has the main package\'s version');
assert.ok(require.cache[addon], `the addon was loaded from ${platform}`);

const c = scxml.chart();
const toggle = c.scxml('toggle', { initial: 'inactive', datamodel: 'ecmascript' },
  c.datamodel({ count: 0 }),
  c.state('inactive', c.transition('TOGGLE', 'active', c.assign('count', 'count + 1'))),
  c.state('active', c.transition('TOGGLE', 'inactive'), c.transition('STOP', 'done')),
  c.final('done', c.donedata(c.param('toggles', 'count'))));
for (const ecmascript of ['native', 'quickjs']) {
  const system = createSystem({ ecmascript });
  const actor = createActor(toggle, { system }).start();
  actor.send({ type: 'TOGGLE' });
  assert.equal(actor.getSnapshot().value, 'active');
  actor.send({ type: 'TOGGLE' });
  actor.send({ type: 'TOGGLE' });
  actor.send({ type: 'STOP' });
  const snapshot = actor.getSnapshot();
  assert.equal(snapshot.status, 'done');
  assert.deepEqual(snapshot.output, { toggles: 2 });
  system.stop();
}

const engine = typeof Bun === 'undefined' ? `node ${process.version}` : `bun ${Bun.version}`;
console.log(`smoke passed: @tinyactors/node ${version} on ${engine}, ${process.platform}-${process.arch}, addon from ${platform}`);
