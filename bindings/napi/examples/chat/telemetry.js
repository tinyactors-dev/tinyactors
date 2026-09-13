"use strict";

const { context, SpanStatusCode, trace } = require("@opentelemetry/api");
const { OTLPTraceExporter } = require("@opentelemetry/exporter-trace-otlp-http");
const {
  BatchSpanProcessor,
  NodeTracerProvider,
} = require("@opentelemetry/sdk-trace-node");
const { resourceFromAttributes } = require("@opentelemetry/resources");

function endpoint() {
  const value = process.env.OTEL_EXPORTER_OTLP_TRACES_ENDPOINT ||
    process.env.OTEL_EXPORTER_OTLP_ENDPOINT ||
    "http://127.0.0.1:4318";
  return value.endsWith("/v1/traces") ? value : value.replace(/\/$/, "") + "/v1/traces";
}

function createTelemetry() {
  if (!process.env.OTEL_EXPORTER_OTLP_TRACES_ENDPOINT &&
      !process.env.OTEL_EXPORTER_OTLP_ENDPOINT) {
    return { trace: undefined, ingress: undefined, workflow: undefined, shutdown: async () => {} };
  }
  const provider = new NodeTracerProvider({
    resource: resourceFromAttributes({
      "service.name": process.env.OTEL_SERVICE_NAME || "tinyactors-chat",
    }),
    spanProcessors: [
      new BatchSpanProcessor(new OTLPTraceExporter({
        url: endpoint(),
        timeoutMillis: Number(process.env.OTEL_EXPORTER_OTLP_TIMEOUT || 1000),
      })),
    ],
  });
  const tracer = provider.getTracer("tinyactors.chat");
  const spans = new WeakMap();
  const capturePayload = process.env.OTEL_TINYACTORS_CAPTURE_PAYLOAD === "true";

  function spanContext(nativeContext) {
    if (!nativeContext) return undefined;
    const traceId = nativeContext.trace_id?.toString("hex");
    const spanId = nativeContext.span_id?.toString("hex");
    if (traceId?.length !== 32 || spanId?.length !== 16) return undefined;
    return {
      traceId,
      spanId,
      traceFlags: nativeContext.trace_flags,
      isRemote: Boolean(nativeContext.remote),
    };
  }

  function nativeContext(span) {
    const value = span.spanContext();
    return {
      trace_id: Buffer.from(value.traceId, "hex"),
      span_id: Buffer.from(value.spanId, "hex"),
      trace_flags: value.traceFlags,
      remote: 0,
    };
  }

  function startHostSpan(name, attributes, parent) {
    const span = tracer.startSpan(
      name,
      { attributes },
      parent ? trace.setSpanContext(context.active(), spanContext(parent)) : undefined,
    );
    return { context: nativeContext(span), end: () => span.end() };
  }

  return {
    ingress(name, attributes = {}) {
      return startHostSpan(name, attributes);
    },
    workflow(name, parent, attributes = {}) {
      return startHostSpan(name, attributes, parent);
    },
    trace: {
      start(info) {
        const explicitParent = spanContext(info.parent);
        const nativeLinks = (info.links || [])
          .map((link) => spanContext(link))
          .filter(Boolean);
        // Keep the native links, but also project the first causal delivery
        // link into a parent for this example's logical workflow tree. This
        // is a visualization aid; links remain the authoritative async edge.
        const parent = explicitParent || nativeLinks[0];
        const links = nativeLinks.map((spanContext) => ({
          context: spanContext,
        }));
        const operation = ["send", "process", "invoke", "upgrade"][info.operation] || "operation";
        const definition = info.definition_name || "unknown";
        const event = info.event_name || "unknown";
        const payload = info.event_data?.native ?? info.event_data?.bytes;
        const attributes = {
          "tinyactors.operation": operation,
          "tinyactors.kind": String(info.kind),
          "tinyactors.actor.id": String(info.actor),
          "tinyactors.actor.definition": definition,
          "tinyactors.message.id": String(info.message_id),
          "tinyactors.message.type": event,
          "tinyactors.destination": info.destination || "",
          "tinyactors.processor": info.processor || "",
        };
        if (capturePayload && payload !== undefined) {
          try {
            attributes["tinyactors.message.payload"] =
              typeof payload === "string" ? payload : JSON.stringify(payload);
          } catch {
            attributes["tinyactors.message.payload"] = "[unserializable]";
          }
        }
        const span = tracer.startSpan(
          `${definition} ${operation} ${event}`,
          {
          parent,
          links,
            attributes,
          },
          parent ? trace.setSpanContext(context.active(), parent) : undefined,
        );
        const context = span.spanContext();
        const handle = {};
        spans.set(handle, span);
        return {
          handle,
          context: {
            trace_id: Buffer.from(context.traceId, "hex"),
            span_id: Buffer.from(context.spanId, "hex"),
            trace_flags: context.traceFlags,
            remote: 0,
          },
        };
      },
      end(info) {
        const span = spans.get(info.handle);
        if (!span) return;
        if (info.status && info.status !== 0) {
          span.setStatus({ code: SpanStatusCode.ERROR });
          span.setAttribute("tinyactors.status", String(info.status));
        }
        span.end();
      },
    },
    async shutdown() {
      // Do not make application shutdown depend on a collector being reachable.
      await Promise.race([
        provider.shutdown(),
        new Promise((resolve) => setTimeout(resolve, 1500)),
      ]);
    },
  };
}

module.exports = { createTelemetry };
