/* The loop (AGENTS.md, "The loop"; tinyactors-core/docs/bindings.md,
 * section 3): take, handle, ack, run. This is its start, the drain the
 * host's creation needs to read its host.config REPLY; the turn, the event
 * loop's driving and the dispatch of every effect come with the loop and
 * the effects (docs/plan.xml, p1-loop and p1-effects). */
#include "addon.h"

/* A take: the drain's state and the take's records, until the ack. */
typedef struct ta_napi_batch {
    ta_napi_drain_state *state;
    const ta_host_effect *effects;
} ta_napi_batch;

/* The REPLY's WIRE kept for the caller (the creation's host.config), as a
 * value read before the ack: the bytes end there. Other effects cannot
 * come before an image is added; until the effects are dispatched they
 * are acknowledged unread. */
static bool ta_napi_dispatch(napi_env env, const ta_napi_batch *batch, uint32_t index) {
    ta_napi_drain_state *state = batch->state;
    ta_host *host = state->host->host;
    if (batch->effects[index].kind != TA_HOST_EFFECT_REPLY) return true;
    napi_value reply = ta_napi_value_read(env, ta_host_effect_bytes(host, index, TA_EFFECT_FIELD_WIRE));
    if (reply == NULL) return false;
    state->reply = reply;
    return true;
}

bool ta_napi_drain(napi_env env, ta_napi_drain_state *state) {
    ta_napi_host *host = state->host;
    ta_napi_batch batch = {state, NULL};
    uint32_t count;
    bool ok = true;
    while ((count = ta_host_effects(host->host, &batch.effects)) > 0) {
        for (uint32_t i = 0; i < count && ok; i++) ok = ta_napi_dispatch(env, &batch, i);
        ta_host_effects_ack(host->host); /* everything handed out for the batch ends */
        host->acks++;                    /* the batch's thin views are stale from here */
        if (!ok) return false;
    }
    return true;
}
