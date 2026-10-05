'use strict';

// parameterInfo() (napi ADR 0035): each parameter names its owner. Core's
// are "core", the host kernel's "kernel" (every binding over the kernel has
// them), and the addon's own "napi".

const assert = require('node:assert/strict');
const { parameterInfo, createSystem } = require('..');

const info = parameterInfo();
const owners = (owner) => Object.keys(info).filter((name) => info[name].owner === owner).sort();

for (const name of ['microstepLimit', 'interpreterBytes', 'maxActors']) {
  assert.equal(info[name].owner, 'core', name);
}
for (const name of ['telBytes', 'maxRecordBytes', 'telemetryValueBytes']) {
  assert.equal(info[name].owner, 'core', name);
}
for (const name of ['messageBudget', 'maxQueuedMessages', 'maxRetainedBytes']) {
  assert.equal(info[name].owner, 'kernel', name);
}
assert.deepEqual(owners('napi'), ['datamodelNameBytes', 'registeredDatamodels', 'traceStagingBytes']);
for (const [name, entry] of Object.entries(info)) {
  assert.ok(['core', 'kernel', 'napi'].includes(entry.owner), `${name}: ${entry.owner}`);
  assert.ok(entry.min <= entry.default && entry.default <= entry.max, name);
}

// Every parameter the table names is one a system accepts and reports.
const system = createSystem({ clock: 'manual' });
assert.deepEqual(Object.keys(system.parameters).sort(), Object.keys(info).sort());
system.close();

console.log('params: ok');
