'use strict';

// Chart codecs (napi ADR 0027, core ADR 0053): registerCodec(), define(text,
// { format, identity }), Definition.encode(), ChartCodecError.

const assert = require('node:assert/strict');
const { createSystem, statechart, ChartCodecError, TinyactorsError } = require('..');

const utf8 = new TextDecoder();

function assertCode(fn, code, pattern) {
  assert.throws(fn, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, code, error.message);
    if (pattern) assert.match(error.message, pattern);
    return true;
  });
  return (() => { try { fn(); } catch (error) { return error; } })();
}

// A small chart syntax: JSON, one state per line after the first, each
// { id, on: [{ event, target }] }. It knows where it read each state, and
// says where it wrote each one.
const chartJSON = {
  name: 'application/vnd.tinyactors-test.chart+json',
  extensions: ['chart.json'],
  description: 'A chart as JSON, for the tests',
  contexts: [],
  decode(bytes, context) {
    chartJSON.contexts.push(context);
    let document;
    try {
      document = JSON.parse(utf8.decode(bytes));
    } catch (error) {
      throw new ChartCodecError('syntax', error.message, { line: 1, column: 1, offset: 0 });
    }
    if (!Array.isArray(document.states)) {
      throw new ChartCodecError('not-chart', 'a chart has states', { line: 1, column: 2, offset: 1, construct: 'chart', attribute: 'states' });
    }
    return {
      name: document.name,
      initial: { targets: [document.states[0].id] },
      states: document.states.map((state, index) => ({
        kind: 'atomic',
        id: state.id,
        span: index + 1,
        transitions: (state.on ?? []).map((on) => ({ kind: 'external', event: on.event, targets: [on.target] })),
      })),
      spans: document.states.map((_, index) => ({ source: 0, line: index + 2, column: 1 })),
      sources: [{ name: context.name ?? '' }],
    };
  },
  encode(spec, context) {
    chartJSON.contexts.push(context);
    const lines = [`{"name":${JSON.stringify(spec.name ?? '')},"states":[`];
    const spans = [];
    spec.states.forEach((state, index) => {
      const on = (state.transitions ?? []).map((transition) => ({ event: transition.event, target: transition.targets[0] }));
      lines.push(`${JSON.stringify({ id: state.id, on })}${index + 1 < spec.states.length ? ',' : ''}`);
      if (state.span) spans[state.span - 1] = { line: lines.length, column: 1, end_line: lines.length, end_column: 2 };
    });
    lines.push(']}');
    return { bytes: lines.join('\n'), spans };
  },
};

const TOGGLE = [
  '{"name":"toggle","states":[',
  '{"id":"off","on":[{"event":"flip","target":"on"}]},',
  '{"id":"on","on":[{"event":"flip","target":"off"}]}',
  ']}',
].join('\n');

function javascriptCodec() {
  const system = createSystem();
  system.registerCodec(chartJSON);
  assert.deepEqual(system.codecs().map((codec) => codec.name), ['application/scxml+xml', chartJSON.name]);
  assert.deepEqual(system.codecs()[1], {
    name: chartJSON.name, extensions: ['chart.json'], description: 'A chart as JSON, for the tests', decodes: true, encodes: true,
  });

  // By its extension or its name; the spans it read name define()'s `name`.
  const resolve = () => undefined;
  chartJSON.contexts.length = 0;
  const toggle = system.define(TOGGLE, { format: 'chart.json', name: 'toggle.chart.json', resolve });
  assert.deepEqual(chartJSON.contexts, [{ name: 'toggle.chart.json', resolve }]);
  assert.deepEqual(toggle.sourceOf('state', 1), { source: 'toggle.chart.json', line: 3, column: 1 });
  assert.equal(toggle.findState('on').id, 1);

  // What it writes, and where; defining that answers the same definition.
  chartJSON.contexts.length = 0;
  const encoded = toggle.encode(chartJSON.name);
  assert.equal(chartJSON.contexts[0].definition.id, toggle.id);
  assert.equal(utf8.decode(encoded.bytes), TOGGLE);
  assert.equal(encoded.identity, undefined);
  assert.deepEqual(encoded.placeOf('state', 0), { source: '', line: 2, column: 1, endLine: 2, endColumn: 2 });
  assert.equal(encoded.placeOf('transition', 0), undefined);
  assert.equal(system.define(encoded.bytes, { format: 'chart.json' }).id, toggle.id);

  // An explicit identity is carried beside the bytes.
  const named = system.define(TOGGLE, { format: 'chart.json', identity: { family: 'switch', revision: 'r1' } });
  assert.notEqual(named.id, toggle.id);
  assert.deepEqual(named.inspect().identity, { family: 'switch', revision: 'r1' });
  const carried = named.encode('chart.json');
  assert.deepEqual(carried.identity, { family: 'switch', revision: 'r1' });
  assert.equal(system.define(carried.bytes, { format: 'chart.json', identity: carried.identity }).id, named.id);

  // The chart runs.
  const actor = system.spawn(toggle);
  return system.flush().then(() => {
    actor.post('flip');
    return system.flush();
  }).then(() => {
    assert.deepEqual(actor.inspect().activeStates, ['on']);
    system.close();
  });
}

function refusals() {
  const system = createSystem();
  system.registerCodec(chartJSON);
  const syntax = assertCode(() => system.define('{', { format: 'chart.json' }), 'invalid', /at line 1, column 1/);
  assert.equal(syntax.detail.stage, 'decode');
  assert.equal(syntax.detail.format, chartJSON.name);
  assert.equal(syntax.detail.code, 'syntax');
  assert.equal(syntax.detail.offset, 0);
  const notChart = assertCode(() => system.define('{}', { format: 'chart.json' }), 'invalid');
  assert.deepEqual(
    { code: notChart.detail.code, construct: notChart.detail.construct, attribute: notChart.detail.attribute,
      line: notChart.detail.line, column: notChart.detail.column },
    { code: 'not-chart', construct: 'chart', attribute: 'states', line: 1, column: 2 },
  );
  // A refusal with no place has none.
  system.registerCodec({ name: 'placeless', decode() { throw new ChartCodecError('limit', 'too big'); } });
  const placeless = assertCode(() => system.define('x', { format: 'placeless' }), 'invalid');
  assert.equal(placeless.detail.code, 'limit');
  assert.equal(placeless.detail.line, undefined);
  // A spec the generated layer refuses names what it refused.
  system.registerCodec({ name: 'bad-spec', decode() { return { states: 5 }; } });
  assertCode(() => system.define('x', { format: 'bad-spec' }), 'invalid', /states/);
  system.registerCodec({ name: 'no-spec', decode() { return 5; } });
  assertCode(() => system.define('x', { format: 'no-spec' }), 'invalid', /no spec/);
  // Core's compile error, from a codec's spec.
  system.registerCodec({ name: 'dangling', decode() { return { states: [{ kind: 'atomic', id: 'a', transitions: [{ kind: 'external', targets: ['nowhere'] }] }] }; } });
  assert.equal(assertCode(() => system.define('x', { format: 'dangling' }), 'invalid').detail.stage, 'compile');
  // Any other exception propagates as it is, from decode and from encode.
  const toggle = system.define(TOGGLE, { format: 'chart.json' });
  system.registerCodec({ name: 'thrower', decode() { throw new RangeError('boom'); }, encode() { throw new RangeError('bang'); } });
  assert.throws(() => system.define('x', { format: 'thrower' }), RangeError);
  assert.throws(() => toggle.encode('thrower'), RangeError);
  // A codec may not call back into the system.
  system.registerCodec({ name: 'reentrant', decode() { return system.define(TOGGLE, { format: 'chart.json' }); } });
  assertCode(() => system.define('x', { format: 'reentrant' }), 'reentrant');
  // An encode's refusal.
  system.registerCodec({ name: 'refuser', encode() { throw new ChartCodecError('unrepresentable', 'no', { construct: 'state' }); } });
  const refused = assertCode(() => toggle.encode('refuser'), 'invalid', /cannot write/);
  assert.deepEqual(refused.detail, { message: 'no in <state>', format: 'refuser', code: 'unrepresentable', construct: 'state' });
  system.registerCodec({ name: 'nonsense', encode() { return 5; } });
  assertCode(() => toggle.encode('nonsense'), 'invalid', /neither bytes nor a string/);
  system.close();
}

function registry() {
  const system = createSystem();
  system.registerCodec(chartJSON);
  // A name and an extension share one space; the built-in codec keeps its own.
  assertCode(() => system.registerCodec({ name: chartJSON.name, decode() {} }), 'invalid', /already/);
  assertCode(() => system.registerCodec({ name: 'other', extensions: ['chart.json'], decode() {} }), 'invalid', /already/);
  assertCode(() => system.registerCodec({ name: 'chart.json', decode() {} }), 'invalid', /already/);
  assertCode(() => system.registerCodec({ name: 'application/scxml+xml', decode() {} }), 'invalid', /already/);
  assertCode(() => system.registerCodec({ name: 'scxml', decode() {} }), 'invalid', /already/);
  // What a codec is.
  assertCode(() => system.registerCodec({ name: '' , decode() {} }), 'invalid');
  assertCode(() => system.registerCodec({ name: 'none' }), 'invalid', /decodes, encodes/);
  assertCode(() => system.registerCodec({ name: 'dot', extensions: ['.x'], decode() {} }), 'invalid');
  assertCode(() => system.registerCodec({ name: 'bad', decode: 5 }), 'invalid');
  assertCode(() => new ChartCodecError('nonsense', 'x'), 'invalid');
  assertCode(() => new ChartCodecError('syntax', 'x', { line: -1 }), 'invalid');
  // Formats.
  assertCode(() => system.define('x', { format: 'yaml' }), 'invalid', /no codec is named "yaml"/);
  system.registerCodec({ name: 'reads', decode: chartJSON.decode });
  system.registerCodec({ name: 'writes', encode: chartJSON.encode });
  assertCode(() => system.define(TOGGLE, { format: 'writes' }), 'unsupported', /only writes/);
  const toggle = system.define(TOGGLE, { format: 'reads' });
  assertCode(() => toggle.encode('reads'), 'unsupported', /only reads/);
  assert.equal(utf8.decode(toggle.encode('writes').bytes), TOGGLE);
  system.close();
  assertCode(() => system.registerCodec({ name: 'late', decode() {} }), 'closed');
  // Each system has its own codecs.
  const other = createSystem();
  assert.deepEqual(other.codecs().map((codec) => codec.name), ['application/scxml+xml']);
  other.close();
}

const DOCUMENT = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="door" initial="closed">
  <state id="closed"><transition event="open" target="opened"/></state>
  <state id="opened"><transition event="close" target="closed"/></state>
</scxml>`;

function scxml() {
  const system = createSystem();
  const door = system.define(DOCUMENT, { format: 'scxml' });
  assert.equal(system.define(DOCUMENT, { format: 'application/scxml+xml' }).id, door.id);
  const encoded = door.encode('scxml');
  assert.match(utf8.decode(encoded.bytes), /<state id="opened">/);
  assert.equal(encoded.placeOf('state', 1).line, utf8.decode(encoded.bytes).split('\n').findIndex((line) => line.includes('id="opened"')) + 1);
  assert.equal(system.define(encoded.bytes, { format: 'scxml' }).id, door.id);
  // identity works for SCXML too.
  const named = system.define(DOCUMENT, { format: 'scxml', identity: { family: 'gate', revision: new Uint8Array([0xff, 1]) } });
  assert.deepEqual(named.inspect().identity, { family: 'gate', revision: new Uint8Array([0xff, 1]) });
  const carried = named.encode('scxml');
  assert.equal(system.define(carried.bytes, { format: 'scxml', identity: carried.identity }).id, named.id);
  assertCode(() => system.define(DOCUMENT, { format: 'scxml', identity: { family: 'gate' } }), 'invalid', /identity/);
  // SCXML cannot write a function.
  const builder = system.define(statechart({ family: 'counter', revision: '1' })
    .data('n', 0)
    .state('a', (state) => state.on('go', (transition) => transition.when(function () { return this.n > 0; }).target('a'))));
  const unrepresentable = assertCode(() => builder.encode('scxml'), 'invalid', /cannot write/);
  assert.equal(unrepresentable.detail.code, 'unrepresentable');
  assert.equal(unrepresentable.detail.format, 'application/scxml+xml');
  // A chart from a JavaScript codec writes as SCXML, and back.
  system.registerCodec(chartJSON);
  const toggle = system.define(TOGGLE, { format: 'chart.json' });
  assert.equal(system.define(toggle.encode('scxml').bytes, { format: 'scxml' }).inspect().stateCount, 2);
  // A freed definition encodes no more.
  const gone = system.define(DOCUMENT.replace('door', 'gone'), { format: 'scxml' });
  gone.retire();
  assertCode(() => gone.encode('scxml'), 'not-found');
  system.close();
}

javascriptCodec().then(() => {
  refusals();
  registry();
  scxml();
}).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
