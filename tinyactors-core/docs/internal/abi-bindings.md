# Binding notes

## Node / N-API

When Tinyactors is called on the JavaScript thread, plugin callbacks may naturally call N-API. If Tinyactors is run on native worker threads, callbacks that touch JavaScript must marshal to an allowed JS/N-API context. Asynchronous JS work should return from the C callback and later deliver an event.

JavaScript objects remain host-owned data-model values. Session capture does not serialize them; reconstruction receives the existing/restored binding object. A durability add-on may map selected host references to stable application identities.

## CPython

A binding wraps `PyObject *` with correct reference counts. Calls into Python require the appropriate Python thread state/GIL discipline for the Python version/build. Tinyactors core does not acquire it on the binding's behalf.

The data-model instance can be a small binding around an existing Python object graph. Capturing a session therefore does not pickle or copy that graph. Reconstruction is performed under the binding's chosen GIL/thread-state discipline and receives the appropriate `PyObject *` binding.

## CRuby

A binding wraps Ruby values with correct GC visibility/rooting. Calls into Ruby follow GVL rules. Tinyactors itself does not know about the GVL. Session images retain references according to the binding's lifetime rules; they do not serialize arbitrary `VALUE`s.

## Browser/freestanding WASM

Allocator, I/O, time, and optional durability may all be host imports. Imported JS functions can initiate asynchronous browser work and later call exported Tinyactors functions to deliver events. No filesystem or threads are required.

## ESP32

Use a compact allocator/pool, programmatic or generated definitions, a small data model, caller-driven logical time, and RTOS/device-specific I/O adapters. XML and a durability module are both optional.
