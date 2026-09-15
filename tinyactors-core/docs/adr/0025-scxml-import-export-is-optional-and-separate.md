# ADR 0025: SCXML import/export is optional and separate

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML XML remains valuable for interchange, tooling, visualization, and compatibility, but is not needed for the first tiny actor runtime.

## Decision

An importer/exporter, when implemented, is a separate library/tool that maps W3C SCXML syntax to/from the Tinyactors definition model. The core ships without XML initially.

## Consequences

The project can stay tiny while retaining a route to standards interoperability. Importer conformance can be tested independently against the W3C suite.
