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
const { createSystem, statechart, TraceSpans, formatTraceparent } = require('..');
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

async function main() {
  const exporter = new OtlpExporter({ serviceName: 'tinyactors-demo' });
  const spans = new TraceSpans({ onSpan: exporter.onSpan, values: true });
  const system = createSystem();
  system.setTraceSink((records) => spans.write(records));
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
  exporter.onSpan({
    traceId, spanId: requestSpan, name: 'POST /orders', kind: 'internal', startTime: start, endTime: end,
    attributes: { 'http.request.method': 'POST', 'url.path': '/orders' }, events: [], links: [],
    status: { code: 'ok' },
  });
  system.close();
  const sent = await exporter.flush();
  console.log(`exported ${sent} spans of trace ${traceId}`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
