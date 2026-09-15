# Binding notes

## Node / N-API

When Tinyactors is called on the JavaScript thread, plugin callbacks may naturally call N-API. If Tinyactors is run on native worker threads, callbacks that touch JavaScript must marshal to an allowed JS/N-API context. Asynchronous JS work should return from the C callback and later deliver an event.

## CPython

A binding wraps `PyObject *` with correct reference counts. Calls into Python require the appropriate Python thread state/GIL discipline for the Python version/build. The Tinyactors core does not acquire it on the binding's behalf.

## CRuby

A binding wraps Ruby values with correct GC visibility/rooting. Calls into Ruby follow GVL rules. Tinyactors itself does not know about the GVL.

## Browser/freestanding WASM

Allocator, I/O, time, and persistence may all be host imports. Imported JS functions can initiate asynchronous browser work and later call exported Tinyactors functions to deliver events. No filesystem or threads are required.

## ESP32

Use a compact allocator/pool, programmatic or generated definitions, a small data model, caller-driven logical time, and RTOS/device-specific I/O adapters. XML is unnecessary.
