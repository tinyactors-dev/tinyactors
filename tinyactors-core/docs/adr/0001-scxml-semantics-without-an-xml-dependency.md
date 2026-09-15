# ADR 0001: SCXML semantics without an XML dependency

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML 1.0 provides the desired hierarchical, parallel, history, event, invocation, delayed-send, and error semantics. XML is useful as an interchange syntax but would unnecessarily enlarge the first implementation and embedded footprint.

## Decision

TA adopts the SCXML 1.0 **execution model** as its semantic foundation but exposes a C definition/builder representation. The core does not parse XML. An optional importer/exporter may translate between W3C SCXML documents and TA definitions later.

## Consequences

The initial library is not by itself a conforming W3C SCXML Processor, because processor conformance includes accepting SCXML documents. The execution engine can still be kept semantically aligned with the standard. XML, namespaces, XPath, and DOM machinery are not core dependencies.
