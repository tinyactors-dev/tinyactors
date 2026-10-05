#pragma once
/*
 * The kernel's own layout of what tinyactors_host/otlp.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/otlp.h"

/* A growable byte buffer over an allocator. */
typedef struct tah_otlp_buf {
    ta_allocator allocator;
    uint8_t *data;
    size_t len;
    size_t cap;
} tah_otlp_buf;

typedef struct tah_otlp_span {
    uint8_t trace_id[16];
    uint64_t span_id;
    uint64_t parent_span_id; /* 0: none */
    uint8_t link_trace_id[16];
    uint64_t link_span_id;   /* 0: no link */
    uint64_t start;
    uint32_t microstep;      /* for a microstep span */
    bool error;
    uint64_t definition;     /* the definition whose scope the span goes in */
    tah_otlp_buf name;
    tah_otlp_buf attributes; /* encoded Span.attributes entries */
    tah_otlp_buf events;     /* encoded Span.events entries */
} tah_otlp_span;

/* A session's open macrostep, and its open microstep. */
typedef struct tah_otlp_open {
    uint64_t session;
    uint64_t macrostep;
    bool has_micro;
    tah_otlp_span macro;
    tah_otlp_span micro;
} tah_otlp_open;

/* A finished span's place in `spans`, chained to the next of its definition's. */
typedef struct tah_otlp_entry {
    size_t at;
    uint32_t len;
    uint32_t next; /* index + 1 into `entries` of the definition's next span; 0: none */
} tah_otlp_entry;

/*
 * The spans of one definition in the request being built: one ScopeSpans,
 * whose scope names the definition (napi ADR 0028). Its scope is encoded
 * when its first span is written, while the definition is alive.
 */
typedef struct tah_otlp_run {
    uint64_t definition;
    size_t scope_at;   /* its encoded InstrumentationScope, in `scopes` */
    uint32_t scope_len;
    size_t spans;      /* the bytes of its ScopeSpans.spans entries */
    uint32_t first;    /* index + 1 into `entries`; 0: none */
    uint32_t last;
} tah_otlp_run;

/* A macrostep whose spans a drop ended (core ADR 0098): its later records start nothing. */
typedef struct tah_otlp_skip {
    uint64_t session;
    uint64_t macrostep;
} tah_otlp_skip;

typedef struct tah_otlp_name {
    uint64_t definition;
    uint32_t tag; /* what is named: kind in the top two bits, the ID below */
    uint32_t len;
    size_t at;    /* in `names` */
    bool used;
} tah_otlp_name;

struct tah_otlp {
    tah_otlp_config config;
    tah_otlp_open *open;       /* the first `open_count` are open; the rest are spare */
    uint32_t open_count;
    uint32_t open_capacity;
    uint32_t *open_index;      /* open addressing by session: index + 1 into `open`, 0 empty */
    uint32_t open_index_capacity; /* a power of two, at least twice `open_capacity` */
    uint64_t last_now;         /* the time of the last record read: a drop ends open spans at it */
    tah_otlp_span lifecycle;   /* a lifecycle span while it is built */
    tah_otlp_buf logs;         /* encoded ScopeLogs.log_records entries */
    tah_otlp_buf log_body;     /* a log's body while its attributes are built */
    uint32_t log_count;
    /* Rate limits, one slot per (definition, name) hash; a collision shares one. */
    struct {
        uint64_t key;
        uint64_t window; /* the second the count is for */
        uint32_t count;
        uint32_t suppressed;
    } log_rates[256];
    tah_otlp_name *name_slots; /* open addressing, a power of two */
    uint32_t name_capacity;
    uint32_t name_count;
    tah_otlp_buf names;        /* the cached facts' and event names' text */
    tah_otlp_buf resource;     /* encoded Resource.attributes entries */
    /* Encoded ScopeSpans.spans entries that ended since the last put, in
     * the order they ended; `entries` says where each is and chains each
     * definition's, and `runs` holds one per definition, in the order their
     * first span ended (napi ADR 0028). `scope_spans` is the running total
     * of the ResourceSpans.scope_spans fields, which a request's size is
     * computed from. */
    tah_otlp_buf spans;
    tah_otlp_entry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    tah_otlp_run *runs;
    uint32_t run_count;
    uint32_t run_capacity;
    uint32_t *run_index;       /* open addressing by definition: index + 1 into `runs`, 0 empty */
    uint32_t run_index_capacity; /* a power of two, at least twice `run_count` */
    tah_otlp_buf scopes;       /* the runs' encoded InstrumentationScope messages */
    size_t scope_spans;
    tah_otlp_buf scratch;      /* an event's attributes while it is built */
    tah_otlp_buf label;        /* a label while it is built */
    tah_otlp_buf text;         /* an event name or ID while it is formatted */
    uint32_t span_count;       /* spans in `spans` */
    /* Bytes this turn's requests used from `spans` and `logs`, and the
     * typical use of a turn that wrote any (an average that moves an eighth
     * of the way each such turn), for tah_otlp_end_turn(). */
    size_t turn_spans;
    size_t turn_logs;
    size_t typical_spans;
    size_t typical_logs;
    /* The latest sampled macrostep span opened, for the host's exemplars
     * (host_metrics.h); the host clears `set` when it takes it. */
    tah_exemplar last_macrostep;
    uint64_t dropped;          /* records lost to a failed allocation */
    /* Records core dropped on tel, as its reports said (core ADR 0098), and
     * the records the encoder skipped because a drop ended their spans. */
    uint64_t telemetry_dropped;
    uint64_t skipped;
    tah_otlp_skip *skips;
    uint32_t skip_count;
    uint32_t skip_capacity;
};

/* Spans still open (for tests). */
static inline uint32_t tah_otlp_open_count(const tah_otlp *otlp) { return otlp->open_count; }
