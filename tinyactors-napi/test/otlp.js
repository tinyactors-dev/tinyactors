'use strict';

// The test-only OTLP/HTTP exporter (tools/otlp.js, ADR 0015) against an
// in-process receiver: the spans TraceSpans makes of a traced request
// arrive as one OTLP JSON export, in one trace, parented as they ran.
// Runs on Node and Bun.

const assert = require('node:assert/strict');
const http = require('node:http');
const { createSystem, statechart, TraceSpans, formatTraceparent } = require('..');
const { OtlpExporter, exportRequest } = require('../tools/otlp');

const TRACE_ID = '4bf92f3577b34da6a3ce929d0e0e4736';
const REQUEST_SPAN = '00f067aa0ba902b7';

// A collector that keeps every export it receives.
function receiver() {
  const exports = [];
  const server = http.createServer((request, response) => {
    let body = '';
    request.on('data', (chunk) => { body += chunk; });
    request.on('end', () => {
      exports.push({ path: request.url, type: request.headers['content-type'], body: JSON.parse(body) });
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end('{}');
    });
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve({ server, exports, endpoint: `http://127.0.0.1:${server.address().port}` }));
  });
}

(async () => {
  const collector = await receiver();
  const exporter = new OtlpExporter({ endpoint: collector.endpoint, serviceName: 'otlp-test' });
  const system = createSystem();
  const spans = new TraceSpans({ onSpan: exporter.onSpan, system });
  system.setTraceSink((records) => spans.write(records));
  const definition = system.define(
    statechart({ family: 'relay', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.target('busy').raise('next')))
      .state('busy', (s) => s.on('next', (t) => t.target('idle'))),
  );
  const actor = system.spawn(definition);
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: formatTraceparent({ traceId: TRACE_ID, spanId: REQUEST_SPAN }) });
  await system.flush();
  system.close();
  const sent = await exporter.flush();
  collector.server.close();

  assert.equal(collector.exports.length, 1, 'one export');
  const [{ path, type, body }] = collector.exports;
  assert.equal(path, '/v1/traces');
  assert.equal(type, 'application/json');
  const [resourceSpans] = body.resourceSpans;
  assert.deepEqual(resourceSpans.resource.attributes, [{ key: 'service.name', value: { stringValue: 'otlp-test' } }]);
  const received = resourceSpans.scopeSpans[0].spans;
  assert.equal(received.length, sent);
  const traced = received.filter((span) => span.traceId === TRACE_ID);
  const macrostep = traced.find((span) => span.name === 'scxml.macrostep go');
  const micro = traced.filter((span) => span.parentSpanId === macrostep.spanId);
  assert.equal(macrostep.parentSpanId, REQUEST_SPAN, 'the macrostep sits under the request span');
  assert.deepEqual(micro.map((span) => span.name), ['scxml.microstep go', 'scxml.microstep next']);
  assert.equal(macrostep.kind, 1);
  assert.match(macrostep.startTimeUnixNano, /^\d+$/);
  assert.ok(BigInt(macrostep.endTimeUnixNano) >= BigInt(macrostep.startTimeUnixNano));
  const raised = micro[0].events.find((event) => event.name === 'scxml.raise');
  assert.ok(raised, 'the raise is an event of the first microstep');
  assert.deepEqual(raised.attributes.find((attribute) => attribute.key === 'scxml.event.name'),
                   { key: 'scxml.event.name', value: { stringValue: 'next' } });
  // Attribute encoding: arrays, integers, booleans.
  const request = exportRequest([{ traceId: TRACE_ID, spanId: REQUEST_SPAN, name: 'x', startTime: 1, endTime: 2,
                                   attributes: { list: ['a'], count: 3, ok: true, ratio: 0.5 }, events: [], links: [],
                                   status: { code: 'error' } }]);
  const encoded = request.resourceSpans[0].scopeSpans[0].spans[0];
  assert.deepEqual(encoded.attributes, [
    { key: 'list', value: { arrayValue: { values: [{ stringValue: 'a' }] } } },
    { key: 'count', value: { intValue: '3' } },
    { key: 'ok', value: { boolValue: true } },
    { key: 'ratio', value: { doubleValue: 0.5 } },
  ]);
  assert.deepEqual(encoded.status, { code: 2 });
  assert.equal(encoded.startTimeUnixNano, '1000000');
  console.log('otlp.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
