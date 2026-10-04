#include "routing.h"
#include "scxml_uri.h"

#include "interpreter.h"
#include "system.h"
#include "memory.h"
#include "str.h"
#include "../include/tinyactors.h"
#include "text_writer.h"
#include "atom.h"
#include "session.h"
#include "datamodel.h"
#include "actor.h"

/*
 * SCXML 6.2 and C.1 define the target URIs of the SCXML Event I/O
 * Processor; ADR 0044 adds `#_actor_<family>/<name>` for the logical
 * address of a named actor (ADR 0037).
 *
 *   #_internal                 the sender's own internal queue
 *   #_parent                   the invoking parent session
 *   #_scxml_<session>          a session by its session ID (ADR 0066), whose
 *                              text is <incarnation>.<n>.<family> (ADR 0088),
 *                              or a bare <n> a document may write
 *   #_actor_<family>/<name>    a named actor by its logical address, each
 *                              part percent-encoded (ADR 0096)
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

/*
 * ADR 0096: in `#_actor_<family>/<name>` each part is percent-encoded. A
 * byte an RFC 3986 fragment may hold is written as it is, except '/', which
 * separates the parts: the unreserved characters, the sub-delims, ':', '@'
 * and '?'. Every other byte, '/' and '%' among them, is written as %XX
 * with upper-case hex digits.
 */
static bool ta_uri_keeps(uint8_t byte) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) {
        return true;
    }
    switch (byte) {
        case '-': case '.': case '_': case '~':
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
        case ':': case '@': case '?':
            return true;
        default:
            return false;
    }
}

static void ta_uri_put_part(ta_text_writer *writer, ta_str part) {
    static const char hex[] = "0123456789ABCDEF";
    if (part.len == 0) {
        return;
    }
    uint32_t run = 0;
    for (uint32_t index = 0; index < part.len; index += 1) {
        uint8_t byte = (uint8_t)part.ptr[index];
        if (ta_uri_keeps(byte)) {
            continue;
        }
        ta_text_put(writer, (ta_str){ .ptr = part.ptr + run, .len = index - run });
        char escape[3] = { '%', hex[byte >> 4u], hex[byte & 0x0fu] };
        ta_text_put(writer, (ta_str){ .ptr = escape, .len = 3 });
        run = index + 1u;
    }
    ta_text_put(writer, (ta_str){ .ptr = part.ptr + run, .len = part.len - run });
}

void ta_actor_uri_write(ta_text_writer *writer, const ta_actor_address *address) {
    ta_text_put(writer, ta_str_from_cstr(TA_URI_ADDRESS));
    ta_uri_put_part(writer, address->family.name);
    ta_text_put(writer, (ta_str){ .ptr = "/", .len = 1 });
    ta_uri_put_part(writer, address->name);
}

/* The value of a hex digit, or -1. */
static int ta_uri_hex(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

/*
 * Decodes a percent-encoded part into `out` (NULL only measures it) and
 * sets `*length` to its decoded length; false for a '%' not followed by two
 * hex digits. Any other byte is taken as it is, so a document may write a
 * name by hand without encoding what needs no decoding.
 */
static bool ta_uri_decode(ta_str part, char *out, uint32_t *length) {
    uint32_t written = 0;
    for (uint32_t index = 0; index < part.len; index += 1) {
        char byte = part.ptr[index];
        if (byte == '%') {
            if (index + 2u >= part.len) {
                return false;
            }
            int high = ta_uri_hex(part.ptr[index + 1u]);
            int low = ta_uri_hex(part.ptr[index + 2u]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (char)((high << 4) | low);
            index += 2u;
        }
        if (out != NULL) {
            out[written] = byte;
        }
        written += 1u;
    }
    *length = written;
    return true;
}

/*
 * "<family>/<name>", both parts non-empty with no further unencoded slash,
 * interned decoded (ADR 0065). A malformed escape is TA_ERR_INVALID.
 */
static ta_result ta_uri_address_intern(ta_system *system, ta_str text, ta_address **out) {
    uint32_t slash = 0;
    bool encoded = false;
    while (slash < text.len && text.ptr[slash] != '/') {
        encoded = encoded || text.ptr[slash] == '%';
        slash += 1;
    }
    if (slash == 0 || slash + 1u >= text.len) {
        return TA_ERR_INVALID;
    }
    ta_str family = { .ptr = text.ptr, .len = slash };
    ta_str name = { .ptr = text.ptr + slash + 1u, .len = text.len - slash - 1u };
    for (uint32_t index = 0; index < name.len; index += 1) {
        if (name.ptr[index] == '/') {
            return TA_ERR_INVALID;
        }
        encoded = encoded || name.ptr[index] == '%';
    }
    if (!encoded) {
        return ta_address_intern(system, &(ta_actor_address){ .family = { .name = family }, .name = name }, out);
    }
    uint32_t family_length = 0;
    uint32_t name_length = 0;
    if (!ta_uri_decode(family, NULL, &family_length) || !ta_uri_decode(name, NULL, &name_length)) {
        return TA_ERR_INVALID;
    }
    size_t size = (size_t)family_length + name_length;
    char *decoded = ta_mem_alloc(system->allocator, size, 1);
    if (decoded == NULL) {
        return TA_ERR_NOMEM;
    }
    (void)ta_uri_decode(family, decoded, &family_length);
    (void)ta_uri_decode(name, decoded + family_length, &name_length);
    ta_result result = ta_address_intern(
        system,
        &(ta_actor_address){ .family = { .name = { .ptr = decoded, .len = family_length } },
                             .name = { .ptr = decoded + family_length, .len = name_length } },
        out);
    ta_mem_free(system->allocator, decoded, &(ta_allocator_free_params){ .ptr = decoded, .size = size, .alignment = 1 });
    return result;
}

/* Decimal digits only, at least one, no overflow. */
static bool ta_uri_number(ta_str text, uint64_t *out);

/* The digits of `text` up to the first '.', as a number; false without one. */
static bool ta_uri_number_part(ta_str *text, uint64_t *out) {
    uint32_t dot = 0;
    while (dot < text->len && text->ptr[dot] != '.') {
        dot += 1;
    }
    if (dot == text->len || !ta_uri_number((ta_str){ .ptr = text->ptr, .len = dot }, out)) {
        return false;
    }
    text->ptr += dot + 1u;
    text->len -= dot + 1u;
    return true;
}

bool ta_session_text_parse(ta_str text, ta_session_text *out) {
    ta_session_text parsed = {0};
    if (!ta_uri_number_part(&text, &parsed.incarnation) || !ta_uri_number_part(&text, &parsed.session) ||
        text.len == 0) {
        return false;
    }
    parsed.family = text;
    *out = parsed;
    return true;
}

void ta_session_text_write(const ta_system *system, ta_text_writer *writer, uint64_t session, ta_str family) {
    ta_text_put_u64(writer, system->incarnation);
    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
    ta_text_put_u64(writer, session);
    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
    ta_text_put(writer, family);
}

void ta_session_ref_write(const ta_system *system, ta_text_writer *writer, const ta_actor_ref *ref) {
    ta_actor_id runtime = ref->as.runtime;
    uint64_t session = ta_session_id_of(system, runtime);
    ta_str family = { 0 };
    if (ref->family != 0) {
        family = ta_atom_text(&system->atoms, ref->family);
    } else {
        const ta_actor *actor = ta_actor_lookup(system, runtime);
        if (actor != NULL) {
            family = actor->definition->identity.family.name;
        }
    }
    if (family.len == 0) {
        /* A bare ID a host or a document supplied, naming no live actor. */
        ta_text_put_u64(writer, session);
        return;
    }
    ta_session_text_write(system, writer, session, family);
}

ta_result ta_session_id_format(const ta_system *system, const ta_session_id_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL || params->session == 0 ||
        params->family.len == 0 || params->family.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_session_text_write(system, &writer, params->session, params->family);
    return ta_text_finish(&writer, params->needed);
}

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
        /* The target holds a reference to the interned address (ADR 0065). */
        ta_address *named = NULL;
        ta_result result = ta_uri_address_intern(system, rest, &named);
        if (result != TA_OK) {
            return result;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = named } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_SESSION)) {
        uint64_t session = 0;
        ta_atom family = 0;
        ta_session_text parsed;
        if (ta_session_text_parse(rest, &parsed)) {
            /* The family is interned as an address's would be (ADR 0065). */
            ta_result result = ta_atom_intern(&system->atoms, parsed.family, &family);
            if (result != TA_OK) {
                return result;
            }
            /* Another system's session is none here; its family still routes it. */
            session = parsed.incarnation == system->incarnation ? parsed.session : 0;
        } else if (!ta_uri_number(rest, &session)) {
            /* A session URI naming no session is inaccessible, not invalid. */
            session = 0;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_RUNTIME, .family = family,
                                                .as.runtime = session } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_INVOKE) && rest.len != 0) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE };
        return ta_invoke_id_from_text(system, rest, &out->as.invoke);
    }
    return TA_ERR_INVALID;
}

void ta_scxml_target_release(ta_scxml_target *target) {
    if (target != NULL && target->kind == TA_SCXML_TARGET_ACTOR) {
        ta_actor_ref_release(&target->as.actor);
    }
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
                ta_actor_address view = ta_address_view(target->as.actor.as.named);
                ta_actor_uri_write(writer, &view);
                return true;
            }
            /* A runtime reference is written as its session (ADR 0066, ADR 0088). */
            ta_text_put(writer, ta_str_from_cstr(TA_URI_SESSION));
            ta_session_ref_write(system, writer, &target->as.actor);
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

ta_result ta_actor_uri_format(const ta_actor_uri_format_params *params) {
    if (params == NULL || params->needed == NULL || params->address.family.name.len == 0 ||
        params->address.family.name.ptr == NULL || params->address.name.len == 0 ||
        params->address.name.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_actor_uri_write(&writer, &params->address);
    return ta_text_finish(&writer, params->needed);
}
