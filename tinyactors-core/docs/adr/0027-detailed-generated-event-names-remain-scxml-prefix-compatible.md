# ADR 0027: Detailed generated event names remain SCXML-prefix-compatible

**Status:** Accepted  
**Date:** 2026-09-15

## Context

The W3C standard defines broad names such as `error.execution` and explicitly allows platforms to add suffixes. More context is useful for statechart authors and debugging.

## Decision

Tinyactors generates structured detail suffixes where useful, for example `error.execution.datamodel.condition` or `error.communication.send.dispatch`. The standard prefix remains intact, so a transition listening to `error.execution` or simply `error` still catches the event. `error.platform...` remains the application/platform extension namespace.

## Consequences

Statecharts may handle errors at exactly the granularity they need. Detailed names are cheap because names are structural, not repeatedly parsed strings.
