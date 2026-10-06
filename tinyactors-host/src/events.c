#include "events_private.h"

#include "memory.h"

typedef struct {
    uint32_t at;
    uint32_t len;
} tah_name_span;

static uint32_t tah_name_hash(ta_str text) {
    uint32_t hash = 2166136261u;
    for (uint32_t index = 0; index < text.len; index += 1u) {
        hash = (hash ^ (uint8_t)text.ptr[index]) * 16777619u;
    }
    return hash;
}

void tah_names_init(tah_names *names, ta_allocator allocator, uint32_t depth) {
    tah_vec_init(&names->spans, allocator, TAH_LAYOUT(tah_name_span));
    tah_vec_init(&names->bytes, allocator, TAH_LAYOUT(char));
    tah_vec_init(&names->slots, allocator, TAH_LAYOUT(uint32_t));
    names->depth = depth;
}

void tah_names_destroy(tah_names *names) {
    tah_vec_destroy(&names->spans);
    tah_vec_destroy(&names->bytes);
    tah_vec_destroy(&names->slots);
}

static ta_str tah_names_span_text(const tah_names *names, uint32_t index) {
    const tah_name_span *span = tah_vec_at(&names->spans, index);
    return (ta_str){ .ptr = (const char *)names->bytes.items + span->at, .len = span->len };
}

ta_str tah_names_text(const tah_names *names, ta_event_name_id id) {
    uint32_t index = id & ~TAH_NAME_KERNEL;
    if ((id & TAH_NAME_KERNEL) == 0u || index == 0u || index > names->spans.count) {
        return (ta_str){ 0 };
    }
    return tah_names_span_text(names, index - 1u);
}

static bool tah_names_valid(const tah_names *names, ta_str text) {
    if (text.ptr == NULL || text.len == 0) {
        return false;
    }
    uint32_t segments = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= text.len; index += 1u) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index == start || segments == names->depth) {
            return false;
        }
        segments += 1u;
        start = index + 1u;
    }
    return true;
}

/* The slot `text` is in, or the empty slot it would go in. */
static uint32_t *tah_names_slot(const tah_names *names, ta_str text) {
    uint32_t mask = names->slots.count - 1u;
    for (uint32_t at = tah_name_hash(text) & mask;; at = (at + 1u) & mask) {
        uint32_t *slot = tah_vec_at(&names->slots, at);
        if (*slot == 0u) {
            return slot;
        }
        ta_str held = tah_names_span_text(names, *slot - 1u);
        if (held.len == text.len) {
            uint32_t at_byte = 0;
            while (at_byte < text.len && held.ptr[at_byte] == text.ptr[at_byte]) {
                at_byte += 1u;
            }
            if (at_byte == text.len) {
                return slot;
            }
        }
    }
}

/* Doubles the slots (at least 16) and places every name again. */
static ta_result tah_names_rehash(tah_names *names) {
    uint32_t count = names->slots.count == 0u ? 16u : names->slots.count * 2u;
    tah_vec grown;
    tah_vec_init(&grown, names->slots.allocator, TAH_LAYOUT(uint32_t));
    uint32_t empty = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (tah_vec_push(&grown, &empty) != TA_OK) {
            tah_vec_destroy(&grown);
            return TA_ERR_NOMEM;
        }
    }
    tah_vec_destroy(&names->slots);
    names->slots = grown;
    for (uint32_t index = 0; index < names->spans.count; index += 1u) {
        *tah_names_slot(names, tah_names_span_text(names, index)) = index + 1u;
    }
    return TA_OK;
}

ta_result tah_names_intern(tah_names *names, ta_str text, ta_event_name_id *out) {
    if (out == NULL || !tah_names_valid(names, text)) {
        return TA_ERR_INVALID;
    }
    if ((names->spans.count + 1u) * 2u > names->slots.count && tah_names_rehash(names) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    uint32_t *slot = tah_names_slot(names, text);
    if (*slot != 0u) {
        *out = TAH_NAME_KERNEL | *slot;
        return TA_OK;
    }
    if (tah_vec_reserve(&names->bytes, names->bytes.count + text.len) != TA_OK ||
        tah_vec_reserve(&names->spans, names->spans.count + 1u) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    tah_name_span span = { .at = names->bytes.count, .len = text.len };
    tah_mem_copy((char *)names->bytes.items + names->bytes.count, text.ptr, text.len);
    names->bytes.count += text.len;
    (void)tah_vec_push(&names->spans, &span);
    /* The bytes may have moved: find the slot again. */
    slot = tah_names_slot(names, text);
    *slot = names->spans.count;
    *out = TAH_NAME_KERNEL | names->spans.count;
    return TA_OK;
}

ta_result tah_names_adopt(tah_names *names, const ta_system *core, ta_event_name_id *name) {
    if ((*name & TAH_NAME_KERNEL) != 0u || *name == 0u) {
        return TA_OK;
    }
    char stack[256];
    uint32_t needed = 0;
    ta_result result = ta_event_name_format(core, &(ta_event_name_format_params){
                                                      .name = *name, .buffer = stack, .capacity = sizeof stack,
                                                      .needed = &needed });
    if (result == TA_ERR_NOMEM && needed > sizeof stack) {
        tah_vec text;
        tah_vec_init(&text, names->bytes.allocator, TAH_LAYOUT(char));
        if (tah_vec_reserve(&text, needed) != TA_OK) {
            return TA_ERR_NOMEM;
        }
        result = ta_event_name_format(core, &(ta_event_name_format_params){
                                                .name = *name, .buffer = text.items, .capacity = needed,
                                                .needed = &needed });
        if (result == TA_OK) {
            result = tah_names_intern(names, (ta_str){ .ptr = text.items, .len = needed - 1u }, name);
        }
        tah_vec_destroy(&text);
        return result;
    }
    return result == TA_OK ? tah_names_intern(names, (ta_str){ .ptr = stack, .len = needed - 1u }, name) : result;
}

ta_result tah_names_format(const tah_names *names, const ta_system *core, const ta_event_name_format_params *params) {
    if ((params->name & TAH_NAME_KERNEL) == 0u) {
        return ta_event_name_format(core, params);
    }
    ta_str text = tah_names_text(names, params->name);
    if (text.len == 0 || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    *params->needed = text.len + 1u;
    if (params->buffer == NULL || params->capacity < text.len + 1u) {
        return TA_ERR_NOMEM;
    }
    tah_mem_copy(params->buffer, text.ptr, text.len);
    params->buffer[text.len] = '\0';
    return TA_OK;
}

ta_result tah_core_event_name(ta_system *core, ta_str text, ta_event_name_id *out) {
    if (core == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_atom atoms[TA_PARAM_DEFAULT_EVENT_NAME_DEPTH];
    uint64_t depth = TA_PARAM_DEFAULT_EVENT_NAME_DEPTH;
    (void)ta_system_param(core, TA_PARAM_EVENT_NAME_DEPTH, &depth);
    uint32_t count = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= text.len; index += 1) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index == start || count == depth || count == TA_PARAM_DEFAULT_EVENT_NAME_DEPTH) {
            return TA_ERR_INVALID; /* an empty segment, or too many */
        }
        ta_result result = ta_system_intern_atom(core, (ta_str){ .ptr = text.ptr + start, .len = index - start },
                                                 &atoms[count]);
        if (result != TA_OK) {
            return result;
        }
        count += 1u;
        start = index + 1u;
    }
    return ta_event_name_intern(core, &(ta_atom_list){ .atoms = atoms, .count = count }, out);
}
