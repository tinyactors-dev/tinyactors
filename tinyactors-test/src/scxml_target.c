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

bool tat_scxml_target_parse(ta_str target, ta_scxml_target *out, const char **error) {
    static const char actor_prefix[] = "#_scxml_";
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
