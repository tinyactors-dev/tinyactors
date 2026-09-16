# ADR 0013: Snapshot the complete running session

**Status:** Superseded by ADR 0038  
**Date:** 2026-09-15

## Context

The original design treated persistence and interpreter reconstruction as one problem. It therefore required a snapshot to contain both Tinyactors interpreter state and serialized data-model state.

## Historical decision

A complete snapshot included actor/definition identity, name, active configuration, history, data-model state, active invocations, delayed sends, and—when paused—continuation, current external event, and internal event queue. Host values were required to serialize.

## Superseded by ADR 0038

Tinyactors still captures the complete **interpreter** state needed for reconstruction, including paused continuations. The host-owned data-model object graph is no longer part of the image, and core session images are no longer defined as persistence blobs. Portable checkpointing belongs to an optional durability layer.
