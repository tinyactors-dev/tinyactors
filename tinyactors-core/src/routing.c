#include "routing.h"

void ta_address_release(ta_actor_ref *ref, ta_allocator allocator) {
    if (ref->kind != TA_ACTOR_REF_NAMED) {
        return;
    }
    ta_str *texts[2] = { &ref->as.named.family.name, &ref->as.named.name };
    for (uint32_t index = 0; index < 2; index += 1) {
        if (texts[index]->ptr != NULL) {
            ta_mem_free(allocator, (void *)texts[index]->ptr,
                        &(ta_allocator_free_params){ .size = (size_t)texts[index]->len + 1u,
                                                     .alignment = 1 });
        }
        *texts[index] = (ta_str){0};
    }
}

static ta_result ta_address_copy_text(ta_str *text, ta_allocator allocator) {
    if (text->ptr == NULL) {
        return TA_OK;
    }
    char *copy = ta_mem_alloc(allocator, (size_t)text->len + 1u, 1);
    if (copy == NULL) {
        *text = (ta_str){0};
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, text->ptr, text->len);
    copy[text->len] = '\0';
    text->ptr = copy;
    return TA_OK;
}

ta_result ta_address_copy(ta_actor_ref *ref, ta_allocator allocator) {
    if (ref->kind != TA_ACTOR_REF_NAMED) {
        return TA_OK;
    }
    ta_result result = ta_address_copy_text(&ref->as.named.family.name, allocator);
    if (result != TA_OK) {
        /* The family copy left its own text null; the name is still borrowed. */
        ref->as.named.name = (ta_str){0};
        return result;
    }
    result = ta_address_copy_text(&ref->as.named.name, allocator);
    if (result != TA_OK) {
        ta_address_release(ref, allocator);
    }
    return result;
}

ta_result ta_external_delivery_clone(const ta_external_delivery *delivery,
                                     ta_allocator allocator,
                                     ta_external_delivery *out) {
    if (delivery == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_external_delivery copy = *delivery;
    ta_result result = ta_address_copy(&copy.destination, allocator);
    if (result == TA_OK) {
        result = ta_event_clone(&delivery->event, &copy.event);
    }
    if (result != TA_OK) {
        ta_address_release(&copy.destination, allocator);
        return result;
    }
    *out = copy;
    return TA_OK;
}

void ta_external_delivery_release(ta_external_delivery *delivery, ta_allocator allocator) {
    if (delivery == NULL) {
        return;
    }
    ta_address_release(&delivery->destination, allocator);
    ta_event_release(&delivery->event);
}
