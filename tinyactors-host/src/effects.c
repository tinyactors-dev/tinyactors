#include "effects.h"

#include "memory.h"

void tah_effects_init(tah_effects *effects, ta_arena *arena) {
    *effects = (tah_effects){ .arena = arena };
}

void tah_effect_release(tah_effect *effect) {
    if (effect->source.kind == TA_ACTOR_REF_NAMED) {
        ta_address_release(effect->source.as.named);
        effect->source = (ta_actor_ref){0};
    }
    ta_value_release(&effect->value);
    ta_value_release(&effect->target);
    ta_value_release(&effect->params);
    effect->value = (ta_value){0};
    effect->target = (ta_value){0};
    effect->params = (ta_value){0};
    if (effect->kind == TAH_EFFECT_INVOKE_FORWARD) {
        ta_event_release(&effect->forwarded);
        effect->forwarded = (ta_event){0};
    }
}

static bool tah_copy_bytes(ta_arena *arena, const uint8_t *ptr, uint32_t len, const uint8_t **out) {
    if (ptr == NULL || len == 0) {
        *out = ptr;
        return true;
    }
    uint8_t *copy = ta_arena_alloc(arena, len, 1);
    if (copy == NULL) {
        return false;
    }
    tah_mem_copy(copy, ptr, len);
    *out = copy;
    return true;
}

bool tah_effects_push(tah_effects *effects, const tah_effect *effect) {
    tah_effect *copy = effects->closed ? NULL : ta_arena_alloc(effects->arena, sizeof *copy, _Alignof(tah_effect));
    if (copy != NULL) {
        *copy = *effect;
        copy->next = NULL;
        const uint8_t *label = NULL;
        const uint8_t *content_type = NULL;
        const uint8_t *content = NULL;
        const uint8_t *target_text = NULL;
        bool copied =
            tah_copy_bytes(effects->arena, (const uint8_t *)effect->label.ptr, effect->label.len, &label) &&
            tah_copy_bytes(effects->arena, (const uint8_t *)effect->content_type.ptr, effect->content_type.len,
                           &content_type) &&
            tah_copy_bytes(effects->arena, effect->content.ptr, effect->content.len, &content) &&
            tah_copy_bytes(effects->arena, (const uint8_t *)effect->target_text.ptr, effect->target_text.len,
                           &target_text);
        if (copied) {
            copy->label.ptr = (const char *)label;
            copy->content_type.ptr = (const char *)content_type;
            copy->content.ptr = content;
            copy->target_text.ptr = (const char *)target_text;
            if (effects->last != NULL) {
                effects->last->next = copy;
            } else {
                effects->first = copy;
            }
            effects->last = copy;
            effects->count += 1u;
            return true;
        }
    }
    tah_effect dropped = *effect;
    tah_effect_release(&dropped);
    effects->dropped += 1u;
    return false;
}

tah_effect *tah_effects_pop(tah_effects *effects) {
    tah_effect *effect = effects->first;
    if (effect == NULL) {
        return NULL;
    }
    effects->first = effect->next;
    if (effects->first == NULL) {
        effects->last = NULL;
    }
    effects->count -= 1u;
    effect->next = NULL;
    return effect;
}

void tah_effects_close(tah_effects *effects) {
    effects->closed = true;
    tah_effect *effect = NULL;
    while ((effect = tah_effects_pop(effects)) != NULL) {
        tah_effect_release(effect);
    }
}
