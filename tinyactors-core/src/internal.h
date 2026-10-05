/**
 * @file internal.h
 *
 * # The umbrella include
 *
 * Lists every private module header of core, in dependency order. The
 * amalgamation reads this list (its `#include "<module>.h"` lines) and
 * lays out `include/tinyactors.c` in the same order: the public header,
 * then each listed header and its source. A unit that defines only public
 * functions is a source alone (`version.c`), appended after the listed
 * modules' sources; platform files (named `<unit>_<suffix>.c`) are not listed.
 *
 * ## Contract
 *
 * - The public API is `include/tinyactors.h` alone (ADR 0041). There is no
 *   private base header: each unit directly includes the public header
 *   when it uses a public type, and every unit whose symbols it uses, so
 *   each header compiles on its own and never relies on what came before
 *   it here.
 * - Module headers add only layouts and internal functions, and never
 *   redeclare a `TA_API` function. Struct layouts are private and free to
 *   change.
 * - The order is by group: the foundation units (memory, text, the
 *   generic containers, the ring and its records), then names and types,
 *   events and routing, the configuration and history, the data model and
 *   invocations, definitions and the builder, actors and session images,
 *   the system and the interpreter. A new header goes in its group.
 *
 * ## Users
 *
 * Core's sources do not include it. The amalgamation reads it, and
 * `tests/smoke.c` includes it to check that the private headers compile
 * together; the other test programs reach private units through
 * `include/tinyactors.c`.
 */
#pragma once

#include "memory.h"
#include "str.h"
#include "vec.h"
#include "bitset.h"
#include "span.h"
#include "text_writer.h"
#include "hash.h"
#include "handle.h"
#include "layout.h"
#include "arena.h"
#include "list.h"
#include "index_pool.h"
#include "chunks.h"
#include "paged_array.h"
#include "heap.h"
#include "table.h"
#include "fifo.h"
#include "quota.h"
#include "bytes.h"
#include "ring.h"
#include "generated/ring_records.h"
#include "generated/ring_codec.h"
#include "tree.h"
#include "atom.h"
#include "names.h"
#include "type_registry.h"
#include "address.h"
#include "io_origin.h"
#include "session.h"
#include "trie.h"
#include "event.h"
#include "trace.h"
#include "scxml_uri.h"
#include "routing.h"
#include "clock.h"
#include "configuration.h"
#include "history.h"
#include "continuation.h"
#include "timer.h"
#include "datamodel.h"
#include "null_datamodel.h"
#include "io.h"
#include "invoke.h"
#include "definition.h"
#include "builder.h"
#include "export.h"
#include "codec.h"
#include "actor.h"
#include "session_image.h"
#include "system.h"
#include "interpreter.h"
