'use strict';

// A small OTLP/HTTP trace exporter, for tests and the otel-tui demo only
// (napi ADR 0016). It is not part of the package: tinyactors depends on no
// telemetry package. The trace sink's bytes are already an OTLP protobuf
// ExportTraceServiceRequest, so exporting is one POST each; this queues
// them and posts in order. It uses only fetch, which Node and Bun both have.

class OTLPExporter {
  #endpoint;
  #pending = [];

  constructor({ endpoint = 'http://127.0.0.1:4318' } = {}) {
    this.#endpoint = `${endpoint.replace(/\/$/, '')}/v1/traces`;
  }

  /** A trace sink: pass it to System.setTraceSink. */
  sink = (bytes) => {
    this.#pending.push(bytes);
  };

  /** Posts every request queued so far; resolves to how many were posted. */
  async flush() {
    const batch = this.#pending.splice(0);
    for (const bytes of batch) {
      const response = await fetch(this.#endpoint, {
        method: 'POST',
        headers: { 'content-type': 'application/x-protobuf' },
        body: bytes,
      });
      if (!response.ok) {
        throw new Error(`OTLP export failed: HTTP ${response.status}`);
      }
      await response.arrayBuffer();
    }
    return batch.length;
  }
}

module.exports = { OTLPExporter };
