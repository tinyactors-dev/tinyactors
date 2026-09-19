# ADR 0001: XML 1.0 without I/O or a C library

**Status:** Accepted  
**Date:** 2026-09-20

## Context

`tinyactors-core` adopts the SCXML 1.0 execution model but not its XML surface (core ADR 0001), and defers an importer/exporter to a separate component (core ADR 0025). That component needs an XML parser, and every established C option forces a dependency the core spent its whole design avoiding. libxml2 brings its own I/O layer, catalogs, global state, and a long history of entity-expansion and external-entity vulnerabilities. Expat is far smaller and can be fed in chunks, but still requires a C library. Both would undo core ADR 0023, which forbids POSIX, filesystem APIs, sockets, and a system clock.

The pressure runs the other way too: an XML library that opens files is a library that can be talked into opening the wrong file. XXE, billion laughs, quadratic blowup, and external-DTD fetches are all consequences of a parser that is allowed to reach outside its input buffer.

## Decision

txml implements the syntax of XML 1.0 and Namespaces in XML 1.0 under the same constraints as the core.

The library uses `stdint.h`, `stddef.h`, and `stdbool.h` and nothing else. It calls no C library function; the byte loops it needs are written out. `txml_str_equal`, `txml_qname_equal`, and `txml_str_starts_with` exist in the public API because callers would otherwise reach for `strncmp`.

The library performs no I/O of any kind. There is no file handle, no descriptor, no entity resolver, and no callback through which the embedder could supply one. Input arrives only through `txml_parse_feed`, as bytes the caller has already obtained. External entities, external DTD subsets, and XInclude are therefore never fetched — not blocked by a flag, not refused by policy, but absent from the machine. A document that references them is either a document error (`TXML_DOC_UNDECLARED_ENTITY`) or, for the external subset, ignored.

Input is UTF-8. A byte sequence that is not well-formed UTF-8, or that encodes a character XML 1.0 forbids, is `TXML_DOC_ENCODING`. Transcoding from UTF-16, Latin-1, or anything else is an adapter the embedder places in front of `txml_parse_feed`, not a mode of the parser. An XML declaration naming another encoding is a document error rather than a silent reinterpretation.

## Consequences

XXE is impossible by construction rather than defended against, which is a materially different security claim: there is no code path to audit, no default to get wrong, and no configuration an embedder can weaken. The remaining document-controlled costs — nesting, attribute counts, name flooding, internal entity expansion — are bounded by `txml_limits` (ADR 0010) instead.

txml can be built for the same targets as the core: freestanding WASM, ESP32-class firmware, a game's own allocator, or an ordinary server process. It links into a host that has already decided how it reads bytes, exactly as the core links into a host that has already decided how it schedules work.

The cost is that callers must do their own reading and, for non-UTF-8 input, their own transcoding. A host that wants "parse this file" writes that loop itself; the round-trip example in the documentation is about twenty lines of stdio. txml is also not a conforming XML processor in the sense of accepting every conforming document, because a conforming processor may be asked to read external entities. ADR 0012 states the conformance scope plainly rather than implying it.
