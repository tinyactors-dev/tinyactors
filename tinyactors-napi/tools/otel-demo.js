'use strict';

// Sends one traced request through three actors to an OTLP/HTTP collector,
// such as otel-tui, so its trace can be looked at (ADR 0015). Run it with
// `mise run otel-demo` while otel-tui runs. OTEL_EXPORTER_OTLP_ENDPOINT
// picks the collector (default http://localhost:4318).
//
// The flow: a gateway takes an order, asks the inventory to reserve and
// the payments to charge, and confirms once both answered. The request
// arrives with a traceparent, as an HTTP handler would pass it on, so every
// macrostep it causes lands in one trace under the request's span.

const { randomBytes } = require('node:crypto');
const { createSystem, statechart, formatTraceparent } = require('..');
const { OtlpExporter } = require('./otlp');

function charts(system) {
  const gateway = system.define(
    statechart({ family: 'gateway', revision: 'v1' })
      .data('answers', 0)
      .state('idle', (s) =>
        s.on('order', (t) =>
          t
            .target('waiting')
            .send('reserve', (m) => m.to('#_actor_inventory/main').data(({ event }) => event.data))
            .send('charge', (m) => m.to('#_actor_payments/main').data(({ event }) => event.data)),
        ),
      )
      .state('waiting', (s) => {
        s.on('reserved', (t) => t.assign('answers', function () { return this.answers + 1; }));
        s.on('charged', (t) => t.assign('answers', function () { return this.answers + 1; }));
        s.always((t) => t.when(({ data }) => data.answers === 2).target('confirmed'));
      })
      .state('confirmed'),
  );
  const inventory = system.define(
    statechart({ family: 'inventory', revision: 'v1' })
      .data('reserved', 0)
      .state('ready', (s) =>
        s.on('reserve', (t) =>
          t
            .assign('reserved', function ({ event }) { return this.reserved + event.data.items; })
            .send('reserved', (m) => m.to('#_actor_gateway/main')),
        ),
      ),
  );
  const payments = system.define(
    statechart({ family: 'payments', revision: 'v1' })
      .state('ready', (s) =>
        s.on('charge', (t) => t.target('charging').send('settle', (m) => m.to('#_actor_payments/main').after(5))),
      )
      .state('charging', (s) => s.on('settle', (t) => t.target('ready').send('charged', (m) => m.to('#_actor_gateway/main')))),
  );
  return { gateway, inventory, payments };
}

// The host's own request span, as its HTTP handler would export it: one
// span in OTLP JSON, posted beside tinyactors' protobuf.
async function postRequestSpan(endpoint, { traceId, spanId, start, end }) {
  const nanos = (ms) => (BigInt(ms) * 1000000n).toString();
  const body = {
    resourceSpans: [{
      resource: { attributes: [{ key: 'service.name', value: { stringValue: 'tinyactors-demo' } }] },
      scopeSpans: [{
        scope: { name: 'demo-http' },
        spans: [{
          traceId, spanId, name: 'POST /orders', kind: 2, startTimeUnixNano: nanos(start), endTimeUnixNano: nanos(end),
          attributes: [{ key: 'http.request.method', value: { stringValue: 'POST' } },
                       { key: 'url.path', value: { stringValue: '/orders' } }],
          status: { code: 1 },
        }],
      }],
    }],
  };
  const response = await fetch(`${endpoint}/v1/traces`, {
    method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body),
  });
  if (!response.ok) throw new Error(`OTLP export failed: HTTP ${response.status}`);
}

async function main() {
  const endpoint = process.env.OTEL_EXPORTER_OTLP_ENDPOINT ?? 'http://127.0.0.1:4318';
  const exporter = new OtlpExporter({ endpoint });
  const system = createSystem();
  system.setTraceSink(exporter.sink, { values: true, resource: { 'service.name': 'tinyactors-demo' } });
  const { gateway, inventory, payments } = charts(system);
  system.spawn(inventory, { name: 'main' });
  system.spawn(payments, { name: 'main' });
  const entry = system.spawn(gateway, { name: 'main' });
  await system.flush();

  // The request's own span, as the host's HTTP handler would have it.
  const traceId = randomBytes(16).toString('hex');
  const requestSpan = randomBytes(8).toString('hex');
  const start = Date.now();
  await system.send(entry, 'order', { items: 3, amount: 42 },
                    { traceparent: formatTraceparent({ traceId, spanId: requestSpan }) });
  // The payment settles after 5 ms of real time.
  while (system.inspect(entry).activeStates[0] !== 'confirmed') {
    await new Promise((resolve) => setTimeout(resolve, 5));
    await system.flush();
  }
  const end = Date.now();
  system.close();
  const sent = await exporter.flush();
  await postRequestSpan(endpoint, { traceId, spanId: requestSpan, start, end });
  console.log(`exported ${sent} OTLP requests for trace ${traceId}`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
