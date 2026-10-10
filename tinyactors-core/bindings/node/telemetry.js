'use strict';
/**
 * The OpenTelemetry environment variables, read once, as the exporter
 * settings core.mjs takes (createSystem({ telemetry }) wins over them).
 * Only the Node.js entry reads the environment: bindings/shared and the C
 * layers never do.
 *
 *   OTEL_EXPORTER_OTLP_ENDPOINT         base URL; /v1/traces, /v1/logs and /v1/metrics are added
 *   OTEL_EXPORTER_OTLP_TRACES_ENDPOINT  the traces URL as it is (wins over the base)
 *   OTEL_EXPORTER_OTLP_METRICS_ENDPOINT the metrics URL as it is (wins over the base)
 *   OTEL_METRIC_EXPORT_INTERVAL         ms between metrics exports (default 60000)
 *   OTEL_METRICS_EXPORTER               "none": no metrics export
 *   OTEL_EXPORTER_OTLP_HEADERS          key=value,key=value (values percent-encoded)
 *   OTEL_SERVICE_NAME                   resource service.name
 *   OTEL_RESOURCE_ATTRIBUTES            key=value,key=value
 *   OTEL_TRACES_SAMPLER_ARG             the sampling ratio, 0 to 1
 *   OTEL_SDK_DISABLED                   "true": no export
 */

/* W3C Baggage-style lists, as the OpenTelemetry specification reads them */
function keyValues(text) {
  const out = {};
  for (const item of String(text || '').split(',')) {
    const at = item.indexOf('=');
    if (at <= 0) continue;
    const key = item.slice(0, at).trim();
    let value = item.slice(at + 1).trim();
    try {
      value = decodeURIComponent(value);
    } catch {
      /* kept as written */
    }
    if (key) out[key] = value;
  }
  return out;
}

function telemetryFromEnvironment(environment) {
  const telemetry = {};
  const get = (name) => {
    const v = environment[name];
    return typeof v === 'string' && v.trim() ? v.trim() : undefined;
  };
  if ((get('OTEL_SDK_DISABLED') || '').toLowerCase() === 'true') telemetry.disabled = true;
  const traces = get('OTEL_EXPORTER_OTLP_TRACES_ENDPOINT');
  const base = get('OTEL_EXPORTER_OTLP_ENDPOINT');
  if (traces) telemetry.tracesEndpoint = traces;
  if (base) telemetry.endpoint = base;
  if (traces && !base) telemetry.logsEndpoint = ''; /* a traces URL alone: no logs */
  const metrics = get('OTEL_EXPORTER_OTLP_METRICS_ENDPOINT');
  if (metrics) telemetry.metricsEndpoint = metrics;
  if ((get('OTEL_METRICS_EXPORTER') || '').toLowerCase() === 'none') telemetry.metricsEndpoint = '';
  const interval = Number(get('OTEL_METRIC_EXPORT_INTERVAL'));
  if (get('OTEL_METRIC_EXPORT_INTERVAL') && interval >= 0) telemetry.metricsInterval = interval;
  const headers = get('OTEL_EXPORTER_OTLP_HEADERS');
  if (headers) telemetry.headers = keyValues(headers);
  const resource = keyValues(get('OTEL_RESOURCE_ATTRIBUTES'));
  const service = get('OTEL_SERVICE_NAME');
  if (service) resource['service.name'] = service;
  if (Object.keys(resource).length) telemetry.resource = resource;
  const sample = Number(get('OTEL_TRACES_SAMPLER_ARG'));
  if (get('OTEL_TRACES_SAMPLER_ARG') && sample >= 0 && sample <= 1) telemetry.sample = sample;
  return telemetry;
}

module.exports = { telemetryFromEnvironment };
