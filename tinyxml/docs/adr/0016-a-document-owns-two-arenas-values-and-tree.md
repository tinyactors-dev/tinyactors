# ADR 0016: A document owns two arenas: values and tree

**Status:** Accepted  
**Date:** 2026-09-20  
**Supersedes:** the single-arena and borrowed-arena parts of [ADR 0002](0002-one-linked-chunk-arena-per-document-nothing-relocates.md)

## Context

ADR 0002 put everything a document owns into one arena: the columns, the byte pool, and the page directories. That is the simplest thing that can work, and two problems came out of writing the suite against it.

The first is a lifetime conflict. ADR 0013 promised that a rewind keeps name IDs and their pool bytes valid while dropping the nodes above a mark, and `invariants.md` repeated it as invariant 24. With one arena that cannot be true. Names, prefixes and namespace URIs are interned into the same pool as text and attribute values, in parse order, so a name first seen inside the discarded record has its bytes above the mark. A single `txml_arena_release` frees them, and `txml_doc_name_at` then reads freed memory. The promise and the mechanism contradicted each other for any document whose later records introduce a name — a heterogeneous feed, which is exactly the shape streaming existed for.

The second is that ownership was conditional. `txml_parser_config.arena` let a caller supply an arena, and `txml_doc_destroy` then released the document's chunks without destroying it. So "destroying a document frees its memory" had an asterisk, and every lifetime statement in the header had to carry the same asterisk. An embedder reading the API could not answer "who frees this" without knowing how the parser was configured.

Underneath both is a fact the single arena obscured: the things a document stores have two different lifetimes. A name, a prefix, a namespace URI and an attribute value are written once and live as long as the document. Node, attribute and position entries are appended in order and are, in principle, truncatable. Putting them in one bump allocator ties the first group's lifetime to the second's.

## Decision

A document owns **two** arenas, both created over `config.allocator` at `txml_parser_init` and both destroyed by `txml_doc_destroy`:

- The **values arena** holds everything interned and write-once: the byte pool (name local parts, prefixes, namespace URIs, attribute values and text), the names column and the namespaces column, with their page directories.
- The **tree arena** holds the structure: the node, attribute, position and prefix-exception columns and their page directories.

The split is by lifetime, not by type. The test for which arena something belongs in is whether it can ever be discarded independently of the rest of the document: if not, it is a value.

`txml_parser_config.arena` is removed. The embedder still injects the allocator, and `txml_arena` stays public for the embedder's own data, but a document's arenas are the document's. `txml_doc_destroy` destroys both, unconditionally, and that sentence needs no qualifier.

`txml_doc_arena` takes which arena to report:

```c
typedef enum { TXML_ARENA_VALUES, TXML_ARENA_TREE } txml_arena_kind;
TXML_API const txml_arena *txml_doc_arena(const txml_doc *doc, txml_arena_kind which);
```

`limits.max_document_bytes` bounds the **sum** of the two arenas' `used`, so it still means "this document may not cost more than this" and no configuration change is needed to keep an existing budget honest. Pool offsets remain `uint32_t` and now address the values arena alone.

## Consequences

The lifetime conflict is gone by construction rather than by rule. Releasing the tree arena cannot touch a byte any name, namespace or attribute value points at, because those bytes are not in it. Nothing has to remember to preserve them.

Ownership is unconditional. Every `TXML_LIFETIME(document)` annotation in the header now means one thing, and `txml_doc_destroy` is the only answer to "who frees this".

Accounting gets better rather than worse. `used` per arena separates what a document spends on content from what it spends on structure, which is the split an embedder tuning `chunk_size` or `max_document_bytes` actually wants to see. A document that is mostly text and one that is mostly deeply nested empty elements were previously indistinguishable in a single number.

The costs are real and small. Two chunk streams mean two chunk tails of internal waste instead of one, still visible as `reserved` minus `used` and still tuned by `chunk_size`, which applies to both. Two arenas also mean two minimum allocations for a document that holds almost nothing, which matters only for the many-tiny-documents case.

The capability actually lost is sharing: an embedder can no longer hand the parser an arena it already owns and keep everything in one region. That was a genuine feature of ADR 0002 and it is being traded for an unconditional destroy and a sound lifetime split. An embedder that needs one region still supplies one allocator, which is where the real control is; the arenas above it are an implementation of the document, not a place for the caller's data.

This also reopens the door ADR 0018 closes. If mark-and-rewind returns, it releases the tree arena to a mark and leaves the values arena alone, which is the design ADR 0013 needed and did not have.
