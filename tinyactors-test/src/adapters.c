#include "allocator.h"
#include "binding.h"
#include "json_datamodel.h"

/*
 * Hand-written adapter factories. A vtable-bearing argument crosses the
 * protocol as {"$adapter": "<name>", "config": {...}}; callbacks never become
 * nested RPC conversations, so every adapter behaves synchronously in C.
 */

static bool tat_no_config(tat_bind *bind, jv config) {
    if (!jv_is_valid(config) || jv_get_kind(config) == JV_KIND_NULL) {
        return true;
    }
    return tat_fail(bind, "this adapter takes no config");
}

static bool tat_make_libc_allocator(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_allocator *)out = tat_libc_allocator();
    return true;
}

static bool tat_make_jq_datamodel(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_datamodel *)out = tat_json_datamodel(&bind->json);
    return true;
}

const tat_adapter TAT_ADAPTERS[] = {
    {"allocator.libc", "ta_allocator", sizeof(ta_allocator), tat_make_libc_allocator},
    {"datamodel.jq", "ta_datamodel", sizeof(ta_datamodel), tat_make_jq_datamodel},
};

const size_t TAT_ADAPTER_COUNT = sizeof(TAT_ADAPTERS) / sizeof(TAT_ADAPTERS[0]);
