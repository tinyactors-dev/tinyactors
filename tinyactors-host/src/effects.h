#pragma once
/*
 * Effects (ROADMAP step 13, ADR 0001 rule 3): what core asks of a host
 * service while it runs, kept until JavaScript can hear it.
 *
 * Core calls the log sink, the I/O processors and the invokers
 * synchronously, inside a core call. JavaScript must not run there, so each
 * adapter (the adapters in adapters/) copies what the call borrowed into an effect and
 * queues it here. The pump hands the queue to JavaScript at the end of the
 * turn, after the last core call (ADR 0007 point 5), in the order core made
 * the calls.
 *
 * Memory (ADR 0014): an effect and the text and bytes it copies live in the
 * turn arena, so a steady stream of effects makes no allocator call. An
 * effect queued outside a turn (a core call made from a JavaScript API call,
 * such as destroy() cancelling an invocation) waits in the arena for the next
 * turn, which the queue asks for; the arena is only reset at the end of a
 * turn, after the queue was drained. What an effect retains (values, a named
 * source's address, a forwarded event) goes back with tah_effect_release().
 *
 * This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef enum {
    TAH_EFFECT_LOG,           /* <log>: a record for the log sink */
    TAH_EFFECT_IO,            /* <send> to a registered I/O processor */
    TAH_EFFECT_INVOKE_START,  /* an invocation of a JavaScript invoker starts */
    TAH_EFFECT_INVOKE_CANCEL, /* ... is cancelled */
    TAH_EFFECT_INVOKE_FORWARD /* ... gets an autoforwarded event */
} tah_effect_kind;

typedef struct tah_effect {
    tah_effect_kind kind;
    struct tah_effect *next;
    /* LOG, IO: the actor that ran the action; a named address is retained. */
    ta_actor_ref source;
    /* LOG: the label, copied. */
    ta_str label;
    /* LOG: has_value and value; IO: value is the data; START: the data. */
    bool has_value;
    ta_value value;
    /* IO: the evaluated targetexpr; START: the evaluated srcexpr. */
    ta_value target;
    /* IO: a literal target; START: a literal src; copied, ptr NULL when none. */
    ta_str target_text;
    /* START: the params. */
    ta_value params;
    /* IO: the send. */
    ta_send_id send_id;
    ta_event_name_id event;
    /* IO: the processor type; START: the invoker type. */
    ta_atom type;
    /* IO, START: literal <content>, copied. */
    ta_str content_type;
    ta_bytes content;
    /* START, CANCEL, FORWARD. */
    ta_actor_id parent;
    ta_invoke_id invoke_id;
    uint64_t service;
    bool autoforward;
    /* FORWARD: the event, retained. */
    ta_event forwarded;
    /* IO, START: the sending or invoking step's trace parent (core ADR 0067),
     * for JavaScript to pass on as a traceparent. */
    ta_trace_parent trace;
} tah_effect;

typedef struct {
    ta_arena *arena;   /* the turn arena */
    tah_effect *first;
    tah_effect *last;
    uint32_t count;
    bool closed;       /* the system is closing: new effects are dropped */
    uint64_t dropped;  /* effects that could not be queued (no memory, or closing) */
} tah_effects;

void tah_effects_init(tah_effects *effects, ta_arena *arena);
/*
 * Queues a copy of `effect`, which hands over the references it holds
 * (values, source address, forwarded event). Copies the label, the content
 * type, the content and a literal target into the arena. False when the effect cannot be
 * queued (no memory, or the queue is closed); its references are then
 * released.
 */
bool tah_effects_push(tah_effects *effects, const tah_effect *effect);
/* The next effect, removed from the queue, or NULL. Release it after use. */
tah_effect *tah_effects_pop(tah_effects *effects);
/* Releases what an effect retains. */
void tah_effect_release(tah_effect *effect);
/* Closes the queue and releases every queued effect. */
void tah_effects_close(tah_effects *effects);
