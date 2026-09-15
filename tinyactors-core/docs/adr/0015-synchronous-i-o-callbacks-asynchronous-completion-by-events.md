# ADR 0015: Synchronous I/O callbacks, asynchronous completion by events

**Status:** Accepted  
**Date:** 2026-09-15

## Context

A forced outbox/effects architecture would be portable but makes common Node, CPython, CRuby, and WASM embeddings more indirect. Those hosts already have natural synchronous native callbacks that can initiate asynchronous work.

## Decision

I/O processor vtables are called synchronously from the interpreter. They may complete work synchronously or initiate asynchronous host work and return immediately. Later completion re-enters TA as an external event. Delayed-send waiting remains in TA, not the I/O adapter.

## Consequences

Bindings stay straightforward on host interpreter threads. If an embedder executes TA from worker threads, it is responsible for marshaling callbacks to the host runtime when required.
