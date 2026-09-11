/* Standalone value/helper checks; these do not require the actor engine. */
#include "../src/tinyactors.h"
#include "../src/tinyactors.h" /* Header constants must survive repeated inclusion. */

typedef char index_width_check[sizeof(TA_INDEX_NONE) == sizeof(uint32_t) ? 1 : -1];
typedef char revision_width_check[sizeof(TA_REVISION_ANY) == sizeof(uint64_t) ? 1 : -1];
typedef char time_width_check[sizeof(TA_TIME_NEVER) == sizeof(uint64_t) ? 1 : -1];
typedef char limit_width_check[sizeof(TA_UNLIMITED) == sizeof(size_t) ? 1 : -1];

/* Enum values and array addresses remain usable in C99 static initializers. */
static const ta_inspect_options_t inspection = {
    .details = TA_INSPECT_DATA | TA_INSPECT_ACTIVE | TA_INSPECT_HISTORY | TA_INSPECT_EVENT
};
static const ta_ioprocessor_desc_t processors[] = {
    { .type = TA_BASIC_HTTP_IOPROCESSOR },
    {0}
};
static const ta_text_t scxml_processor = {
    TA_SCXML_IOPROCESSOR, sizeof(TA_SCXML_IOPROCESSOR) - 1u
};
static const ta_text_t scxml_invoke = {
    TA_SCXML_INVOKE_TYPE, sizeof(TA_SCXML_INVOKE_TYPE) - 1u
};

/* Keep a non-constant input path in the object file, so the freestanding
 * undefined-symbol check covers the scanning loop rather than only folding. */
ta_text_t test_text_from_input(const char *input)
{
    return ta_text_from_cstr(input);
}

int main(void)
{
    const char embedded_nul[] = { 'a', '\0', 'b', '\0' };
    const uint32_t *missing_index = &TA_INDEX_NONE;
    const uint64_t *any_revision = &TA_REVISION_ANY;
    ta_text_t absent = ta_text_from_cstr(NULL);
    ta_text_t empty = ta_text_from_cstr("");
    ta_text_t prefix = test_text_from_input(embedded_nul);
    ta_text_t uri = ta_text_from_cstr(TA_SCXML_IOPROCESSOR);
    ta_definition_binding_t binding = {0};
    ta_run_result_t run = {0};
    ta_actor_limits_t limits = {0};
    ta_state_desc_t state = {0};

    binding.expected_revision = TA_REVISION_ANY;
    run.next_deadline_ns = TA_TIME_NEVER;
    limits.maximum_bytes = TA_UNLIMITED;
    state.parent = TA_INDEX_NONE;
    if (binding.expected_revision != UINT64_MAX || run.next_deadline_ns != UINT64_MAX ||
        limits.maximum_bytes != (size_t)-1 || state.parent != UINT32_MAX) return 1;
    if (*missing_index != UINT32_MAX || *any_revision != UINT64_MAX) return 9;
    if (absent.data != NULL || absent.size != 0u) return 2;
    if (empty.data == NULL || empty.size != 0u) return 3;
    if (prefix.data != embedded_nul || prefix.size != 1u) return 4;
    if (uri.data != scxml_processor.data || uri.size != scxml_processor.size) return 5;
    if (scxml_invoke.size != 27u || inspection.details != 15u) return 6;
    if (processors[0].type != TA_BASIC_HTTP_IOPROCESSOR || processors[1].type != NULL)
        return 7;
    switch (3) {
    case TA_ABI_VERSION: return 0;
    default: return 8;
    }
}
