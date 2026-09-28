'use strict';

// One scope per definition (napi ADR 0028): a trace request holds a
// ScopeSpans for each definition with spans in it, whose attributes name the
// definition by family, revision, ID, name and source; decodeTraces reads
// them, and the definition logs carry the name and source too. Runs on Node
// and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, decodeTraces } = require('..');

const DOOR = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="door" initial="closed">
  <state id="closed"><transition event="open" target="opened"/></state>
  <state id="opened"><transition event="close" target="closed"/></state>
</scxml>`;

// The strings in protobuf bytes, descending into every length-delimited
// field that parses as a message.
function protobufStrings(bytes, out = []) {
  let at = 0;
  const varint = () => {
    let value = 0;
    let scale = 1;
    for (;;) {
      const byte = bytes[at++];
      if (byte === undefined) throw new Error('truncated');
      value += (byte & 0x7f) * scale;
      scale *= 128;
      if ((byte & 0x80) === 0) return value;
    }
  };
  while (at < bytes.length) {
    const wire = varint() & 7;
    if (wire === 0) varint();
    else if (wire === 1) at += 8;
    else if (wire === 5) at += 4;
    else if (wire === 2) {
      const length = varint();
      const body = bytes.subarray(at, at + length);
      at += length;
      try {
        protobufStrings(body, out);
      } catch {
        out.push(new TextDecoder().decode(body));
      }
    } else {
      throw new Error(`wire type ${wire}`);
    }
  }
  return out;
}

async function twoDefinitions() {
  const system = createSystem();
  const batches = [];
  system.setTraceSink((traces) => batches.push(traces), { detail: 'full' });
  const relay = system.define(
    statechart({ family: 'relay', revision: 'v1', name: 'Relay' })
      .state('idle', (s) => s.on('go', (t) => t.target('busy')))
      .state('busy'),
  );
  const door = system.define(DOOR, { format: 'scxml', name: 'file:///charts/door.scxml' });
  const first = system.spawn(relay);
  const second = system.spawn(door);
  system.post(first, 'go');
  system.post(second, 'open');
  await system.flush();
  const scopes = [];
  for (const bytes of batches) {
    const spans = decodeTraces(bytes);
    const mine = [...new Set(spans.map((span) => span.scope))];
    const ids = mine.map((scope) => BigInt(scope.attributes['tinyactors.definition.id']));
    assert.equal(new Set(ids).size, mine.length, 'one scope per definition in a request');
    for (const scope of mine) {
      assert.equal(scope.name, 'tinyactors');
      const id = BigInt(scope.attributes['tinyactors.definition.id']);
      for (const span of spans.filter((each) => each.scope === scope)) {
        assert.equal(BigInt(span.attributes['scxml.definition']), id, 'a span sits in its definition\'s scope');
      }
    }
    scopes.push(...mine);
  }
  assert.ok(batches.some((bytes) => new Set(decodeTraces(bytes).map((span) => span.scope)).size === 2),
            'a request with both definitions\' spans has two scopes');
  const byFamily = Object.fromEntries(scopes.map((scope) => [scope.attributes['tinyactors.definition.family'], scope]));
  const built = byFamily.relay.attributes;
  assert.equal(built['tinyactors.definition.revision'], 'v1');
  assert.equal(BigInt(built['tinyactors.definition.id']), relay.id);
  assert.equal(built['tinyactors.definition.name'], 'Relay');
  assert.equal(built['tinyactors.definition.source'], __filename, 'the file statechart() was called in');
  const imported = byFamily.door.attributes;
  assert.equal(imported['tinyactors.definition.revision'], door.identity.revision);
  assert.equal(BigInt(imported['tinyactors.definition.id']), door.id);
  assert.equal(imported['tinyactors.definition.name'], 'door');
  assert.equal(imported['tinyactors.definition.source'], 'file:///charts/door.scxml');
  system.close();
}

// A revision that is not UTF-8 is bytes, which decodeTraces reads as hex;
// a chart with no name and no spans leaves those attributes out.
async function bareDefinition() {
  const system = createSystem();
  const batches = [];
  system.setTraceSink((traces) => batches.push(traces));
  const bare = system.define({
    identity: { family: { name: 'bare' }, revision: new Uint8Array([0xff, 0xfe]) },
    states: [{ kind: 'atomic', id: 'only' }],
  });
  system.spawn(bare);
  await system.flush();
  const [scope] = [...new Set(batches.flatMap((bytes) => decodeTraces(bytes)).map((span) => span.scope))];
  assert.equal(scope.attributes['tinyactors.definition.family'], 'bare');
  assert.equal(scope.attributes['tinyactors.definition.revision'], 'fffe');
  assert.ok(!('tinyactors.definition.name' in scope.attributes));
  assert.ok(!('tinyactors.definition.source' in scope.attributes));
  system.close();
}

// A definition retired and freed in the turn of its last span is still
// named: the scope is written with its first span, while it is alive.
async function freedInTurn() {
  const system = createSystem();
  const batches = [];
  system.setTraceSink((traces) => batches.push(traces));
  const brief = system.define(statechart({ family: 'brief', revision: 'v1' }).state('a'));
  const actor = system.spawn(brief);
  system.destroy(actor);
  brief.retire();
  assert.throws(() => brief.inspect(), { code: 'not-found' }, 'freed before the sink runs');
  await system.flush();
  const spans = batches.flatMap((bytes) => decodeTraces(bytes));
  assert.ok(spans.length > 0);
  for (const span of spans) {
    assert.equal(span.scope.attributes['tinyactors.definition.family'], 'brief');
    assert.equal(span.scope.attributes['tinyactors.definition.source'], __filename);
  }
  system.close();
}

// The definition logs carry the name and the source as well.
async function definitionLogs() {
  const system = createSystem();
  const logs = [];
  system.setTraceSink((traces, log) => { if (log !== undefined) logs.push(log); }, { logLimit: 100 });
  system.define(DOOR, { format: 'scxml', name: 'file:///charts/door.scxml' });
  system.spawn(system.define(statechart({ family: 'noted', revision: 'v1' }).state('a')));
  await system.flush();
  const text = logs.flatMap((bytes) => protobufStrings(bytes));
  assert.ok(text.includes('tinyactors.definition.name') && text.includes('door'));
  assert.ok(text.includes('tinyactors.definition.source') && text.includes('file:///charts/door.scxml'));
  system.close();
}

(async () => {
  await twoDefinitions();
  await bareDefinition();
  await freedInTurn();
  await definitionLogs();
  console.log('trace-scopes.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
