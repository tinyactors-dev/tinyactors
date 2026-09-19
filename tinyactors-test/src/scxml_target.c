#include "scxml_target.h"

#include <string.h>

static bool tat_target_is(ta_str target, const char *literal) {
    size_t length = strlen(literal);
    return target.len == length && memcmp(target.ptr, literal, length) == 0;
}

static bool tat_target_prefix(ta_str target, const char *prefix) {
    size_t length = strlen(prefix);
    return target.len >= length && memcmp(target.ptr, prefix, length) == 0;
}

/* Decimal digits only; at least one; no overflow. */
static bool tat_target_number(const char *digits, uint32_t length, uint64_t *out) {
    if (length == 0) {
        return false;
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < length; index += 1) {
        char c = digits[index];
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

/* "#_actor_<family>/<name>": both parts are non-empty and hold no slash. */
static bool tat_target_address(ta_str rest, ta_actor_address *out) {
    const char *slash = memchr(rest.ptr, '/', rest.len);
    if (slash == NULL) {
        return false;
    }
    uint32_t family = (uint32_t)(slash - rest.ptr);
    uint32_t name = rest.len - family - 1u;
    if (family == 0 || name == 0 || memchr(slash + 1, '/', name) != NULL) {
        return false;
    }
    *out = (ta_actor_address){.family = {.name = {.ptr = rest.ptr, .len = family}},
                              .name = {.ptr = slash + 1, .len = name}};
    return true;
}

bool tat_scxml_target_parse(ta_str target, ta_scxml_target *out, const char **error) {
    static const char actor_prefix[] = "#_scxml_";
    static const char named_prefix[] = "#_actor_";
    ta_scxml_target decoded = {0};
    if (target.ptr == NULL) {
        target = (ta_str){.ptr = "", .len = 0};
    }
    if (tat_target_is(target, "#_internal")) {
        decoded.kind = TA_SCXML_TARGET_INTERNAL;
    } else if (tat_target_is(target, "#_self")) {
        decoded.kind = TA_SCXML_TARGET_SELF;
    } else if (tat_target_is(target, "#_parent")) {
        decoded.kind = TA_SCXML_TARGET_PARENT;
    } else if (tat_target_prefix(target, named_prefix)) {
        uint32_t skip = (uint32_t)(sizeof(named_prefix) - 1);
        ta_actor_address address;
        if (!tat_target_address((ta_str){.ptr = target.ptr + skip, .len = target.len - skip}, &address)) {
            *error = "invalid named actor target; expected #_actor_<family>/<name>";
            return false;
        }
        decoded.kind = TA_SCXML_TARGET_ACTOR;
        decoded.as.actor.kind = TA_ACTOR_REF_NAMED;
        decoded.as.actor.as.named = address;
    } else if (tat_target_prefix(target, actor_prefix)) {
        uint64_t actor = 0;
        uint32_t skip = (uint32_t)(sizeof(actor_prefix) - 1);
        if (!tat_target_number(target.ptr + skip, target.len - skip, &actor)) {
            *error = "invalid SCXML actor target";
            return false;
        }
        decoded.kind = TA_SCXML_TARGET_ACTOR;
        decoded.as.actor.kind = TA_ACTOR_REF_RUNTIME;
        decoded.as.actor.as.runtime = actor;
    } else if (tat_target_prefix(target, "#_")) {
        uint64_t invoke = 0;
        if (!tat_target_number(target.ptr + 2, target.len - 2, &invoke)) {
            *error = "invalid SCXML invoke target";
            return false;
        }
        decoded.kind = TA_SCXML_TARGET_INVOKE;
        decoded.as.invoke = invoke;
    } else {
        *error = "unsupported SCXML target";
        return false;
    }
    *out = decoded;
    return true;
}

bool tat_scxml_target_decode(const char *text, size_t length, char *buffer, ta_scxml_target *out,
                             const char **error) {
    if (length > TAT_TARGET_TEXT_MAX) {
        *error = "SCXML target is too long";
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    return tat_scxml_target_parse((ta_str){.ptr = buffer, .len = (uint32_t)length}, out, error);
}
