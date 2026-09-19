#include "scxml_uri.h"

#include "interpreter.h"
#include "system.h"

/*
 * SCXML 6.2 and C.1 define the target URIs of the SCXML Event I/O
 * Processor; ADR 0044 adds `#_actor_<family>/<name>` for the logical
 * address of a named actor (ADR 0037).
 *
 *   #_internal                 the sender's own internal queue
 *   #_parent                   the invoking parent session
 *   #_scxml_<session>          a session by its runtime ID
 *   #_actor_<family>/<name>    a named actor by its logical address
 *   #_<invokeid>               an invocation of the sending session
 *
 * A session URI whose text is not a session ID names no session, which is
 * inaccessible rather than invalid (SCXML C.1): it parses as runtime actor
 * 0, which never exists, so the send reports error.communication. Anything
 * else is an invalid target and the send reports error.execution.
 */

static const char TA_URI_INTERNAL[] = "#_internal";
static const char TA_URI_PARENT[] = "#_parent";
static const char TA_URI_SESSION[] = "#_scxml_";
static const char TA_URI_ADDRESS[] = "#_actor_";
static const char TA_URI_INVOKE[] = "#_";

static bool ta_uri_is(ta_str text, const char *literal) {
    return ta_str_equal(text, ta_str_from_cstr(literal));
}

/* Consumes `prefix` from `text`; false when it does not start with it. */
static bool ta_uri_prefix(ta_str *text, const char *prefix) {
    ta_str candidate = ta_str_from_cstr(prefix);
    if (text->len < candidate.len ||
        !ta_str_equal((ta_str){ .ptr = text->ptr, .len = candidate.len }, candidate)) {
        return false;
    }
    text->ptr += candidate.len;
    text->len -= candidate.len;
    return true;
}

/* "<family>/<name>": both parts are non-empty and hold no further slash. */
static bool ta_uri_address(ta_str text, ta_actor_address *out) {
    uint32_t slash = 0;
    while (slash < text.len && text.ptr[slash] != '/') {
        slash += 1;
    }
    if (slash == 0 || slash + 1u >= text.len) {
        return false;
    }
    ta_str name = { .ptr = text.ptr + slash + 1u, .len = text.len - slash - 1u };
    for (uint32_t index = 0; index < name.len; index += 1) {
        if (name.ptr[index] == '/') {
            return false;
        }
    }
    *out = (ta_actor_address){ .family = { .name = { .ptr = text.ptr, .len = slash } }, .name = name };
    return true;
}

/* Decimal digits only, at least one, no overflow. */
static bool ta_uri_number(ta_str text, uint64_t *out) {
    if (text.len == 0) {
        return false;
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < text.len; index += 1) {
        char digit = text.ptr[index];
        if (digit < '0' || digit > '9' || value > (UINT64_MAX - 9u) / 10u) {
            return false;
        }
        value = value * 10u + (uint64_t)(digit - '0');
    }
    *out = value;
    return true;
}

ta_result ta_scxml_target_parse(ta_system *system, ta_str text, ta_scxml_target *out) {
    if (system == NULL || out == NULL || (text.ptr == NULL && text.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (text.ptr == NULL) {
        text = (ta_str){ .ptr = "", .len = 0 };
    }
    if (ta_uri_is(text, TA_URI_INTERNAL)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INTERNAL };
        return TA_OK;
    }
    if (ta_uri_is(text, TA_URI_PARENT)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_PARENT };
        return TA_OK;
    }
    ta_str rest = text;
    if (ta_uri_prefix(&rest, TA_URI_ADDRESS)) {
        ta_actor_address address;
        if (!ta_uri_address(rest, &address)) {
            return TA_ERR_INVALID;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = address } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_SESSION)) {
        uint64_t session = 0;
        /* A session URI naming no session is inaccessible, not invalid. */
        if (!ta_uri_number(rest, &session)) {
            session = 0;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = session } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_INVOKE) && rest.len != 0) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE };
        return ta_invoke_id_from_text(system, rest, &out->as.invoke);
    }
    return TA_ERR_INVALID;
}

bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target) {
    switch (target->kind) {
        case TA_SCXML_TARGET_INTERNAL:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INTERNAL));
            return true;
        case TA_SCXML_TARGET_PARENT:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_PARENT));
            return true;
        case TA_SCXML_TARGET_INVOKE:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INVOKE));
            ta_text_put(writer, ta_invoke_id_text(system, target->as.invoke));
            return true;
        case TA_SCXML_TARGET_ACTOR:
            if (target->as.actor.kind == TA_ACTOR_REF_NAMED) {
                ta_text_put(writer, ta_str_from_cstr(TA_URI_ADDRESS));
                ta_text_put(writer, target->as.actor.as.named.family.name);
                ta_text_put(writer, (ta_str){ .ptr = "/", .len = 1 });
                ta_text_put(writer, target->as.actor.as.named.name);
                return true;
            }
            ta_text_put(writer, ta_str_from_cstr(TA_URI_SESSION));
            ta_text_put_u64(writer, target->as.actor.as.runtime);
            return true;
        default:
            /* SELF is the absent target, which has no URI of its own. */
            return false;
    }
}

ta_result ta_scxml_target_format(const ta_system *system, const ta_scxml_target_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    if (!ta_scxml_uri_write(system, &writer, &params->target)) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    return ta_text_finish(&writer, params->needed);
}
