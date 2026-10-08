/* effects.c — bounded outbox of effects for the host. Internally effects
 * hold string/event references; ta_effect views are materialized when the
 * host asks for them, so relocation never leaves dangling views. */
#include "base/memory.h"
#include "system/system_internal.h"

int ta_system_effect_full(const ta_system *system) {
    return system->effects_length >= system->effects_capacity;
}

ta_effect_record *ta_system_effect_push(ta_system *system, uint32_t kind, uint32_t actor) {
    if (ta_system_effect_full(system)) return (ta_effect_record *)0;
    ta_effect_record *effect =
        &SYSTEM_POINTER(system, effects_relative, ta_effect_record)[system->effects_length++];
    ta_memset(effect, 0, sizeof *effect);
    effect->kind = kind;
    int live = actor != TA_NIL && ta_pool_live(&system->actors, actor);
    effect->actor = live ? ta_pool_handle(&system->actors, actor) : 0;
    effect->serial = live ? ta_actor_at(system, actor)->serial : 0;
    effect->target = effect->event_name = effect->sendid = effect->auxiliary = TA_NIL;
    effect->event = TA_NIL;
    effect->io_processor = TA_NIL;
    effect->datamodel = 0xFF;
    return effect;
}

void ta_system_effect_release(ta_system *system, ta_effect_record *effect) {
    ta_core *core = &system->core;
    ta_shared_string_release(core, effect->target);
    ta_shared_string_release(core, effect->event_name);
    ta_shared_string_release(core, effect->sendid);
    ta_shared_string_release(core, effect->auxiliary);
    if (effect->event != TA_NIL) {
        ta_event_release(core, effect->event); /* also releases the payload it carries */
    } else if (effect->payload && effect->datamodel < system->datamodel_count &&
               system->datamodels[effect->datamodel].vtable->release) {
        system->datamodels[effect->datamodel].vtable->release(
            system->datamodels[effect->datamodel].datamodel, effect->payload);
    }
    effect->event = TA_NIL;
    effect->payload = 0;
    effect->target = effect->event_name = effect->sendid = effect->auxiliary = TA_NIL;
}

uint32_t ta_system_effects(ta_system *system, const ta_effect **out) {
    ta_core *core = &system->core;
    ta_effect_record *records = SYSTEM_POINTER(system, effects_relative, ta_effect_record);
    ta_effect *effect = SYSTEM_POINTER(system, effect_views_relative, ta_effect);
    uint32_t n = system->effects_length - system->effects_head;
    for (uint32_t i = 0; i < n; i++) {
        const ta_effect_record *record = &records[system->effects_head + i];
        ta_effect *x = &effect[i];
        ta_memset(x, 0, sizeof *x);
        x->size = sizeof *x;
        x->kind = record->kind;
        x->actor = record->actor;
        x->token = record->token;
        x->io_processor = record->io_processor;
        x->flags = record->flags;
        x->target = ta_shared_string_get(core, record->target);
        x->event = ta_shared_string_get(core, record->event_name);
        x->sendid = ta_shared_string_get(core, record->sendid);
        x->auxiliary = ta_shared_string_get(core, record->auxiliary);
        x->payload =
            record->event != TA_NIL ? ta_event_at(core, record->event)->payload : record->payload;
        uint32_t datamodel = record->event != TA_NIL ? ta_event_at(core, record->event)->datamodel
                                                     : record->datamodel;
        x->datamodel = x->payload && datamodel < system->datamodel_count ? datamodel : TA_NIL;
        x->delay_hint = record->delay;
        x->session = record->serial;
        x->trace = record->trace;
    }
    *out = effect;
    return n;
}

void ta_system_effects_ack(ta_system *system, uint32_t n) {
    ta_effect_record *effect = SYSTEM_POINTER(system, effects_relative, ta_effect_record);
    uint32_t avail = system->effects_length - system->effects_head;
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; i++)
        ta_system_effect_release(system, &effect[system->effects_head + i]);
    ta_memset(&effect[system->effects_head], 0,
              (size_t)n * sizeof(ta_effect_record)); /* dead entries: never residue */
    system->effects_head += n;
    if (system->effects_head == system->effects_length) {
        system->effects_head = system->effects_length = 0;
        ta_memset(SYSTEM_POINTER(system, effect_views_relative, ta_effect), 0,
                  (size_t)system->effects_capacity * sizeof(ta_effect));
    } else if (system->effects_head > 0) { /* compact so producers always append */
        uint32_t rem = system->effects_length - system->effects_head;
        for (uint32_t i = 0; i < rem; i++)
            effect[i] = effect[system->effects_head + i];
        ta_memset(&effect[rem], 0,
                  (size_t)(system->effects_length - rem) * sizeof(ta_effect_record));
        system->effects_head = 0;
        system->effects_length = rem;
    }
}
