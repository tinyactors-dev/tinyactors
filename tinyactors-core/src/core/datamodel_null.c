/* datamodel_null.c — the SCXML "null" datamodel (SCXML 1.0 Appendix B.1).
 *
 * No data. The only supported expression is the In() predicate, used in
 * cond attributes: In('stateid'). Everything else raises error.execution. */
#include "core/core_internal.h"
#include "base/memory.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Parse  In ( 'id' )  and return the state index, TA_NIL if malformed. */
static uint32_t parse_in(const ta_image *image, uint32_t chart, ta_string_view s) {
    uint32_t i = 0;
    while (i < s.length && is_space(s.pointer[i]))
        i++;
    if (i + 2 > s.length || s.pointer[i] != 'I' || s.pointer[i + 1] != 'n') return TA_NIL;
    i += 2;
    while (i < s.length && is_space(s.pointer[i]))
        i++;
    if (i >= s.length || s.pointer[i++] != '(') return TA_NIL;
    while (i < s.length && is_space(s.pointer[i]))
        i++;
    if (i >= s.length || (s.pointer[i] != '\'' && s.pointer[i] != '"')) return TA_NIL;
    char q = s.pointer[i++];
    uint32_t b = i;
    while (i < s.length && s.pointer[i] != q)
        i++;
    if (i >= s.length) return TA_NIL;
    ta_string_view id = {s.pointer + b, i - b};
    i++;
    while (i < s.length && is_space(s.pointer[i]))
        i++;
    if (i >= s.length || s.pointer[i++] != ')') return TA_NIL;
    while (i < s.length && is_space(s.pointer[i]))
        i++;
    if (i != s.length) return TA_NIL;
    return ta_image_state_by_name(image, chart, id);
}

static ta_status null_create(void *datamodel, const ta_datamodel_start *start, uint64_t *instance) {
    (void)datamodel;
    (void)start;
    *instance = 1;
    return TA_OK;
}

static ta_status null_condition(const ta_datamodel_call *call, ta_expression expression, int *out) {
    ta_datamodel_environment *environment = call->environment;
    const ta_image *image = ta_datamodel_environment_image(environment);
    uint32_t chart = ta_datamodel_environment_chart(environment);
    ta_expression_info info;
    if (ta_image_expression(image, (ta_chart_item){chart, expression}, &info) != TA_OK)
        return TA_ERROR_EXECUTION;
    uint32_t state = parse_in(image, chart, info.source);
    if (state == TA_NIL) return TA_ERROR_EXECUTION;
    *out = ta_datamodel_environment_in(environment, state);
    return TA_OK;
}

static ta_status null_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture,
                              ta_payload *out) {
    (void)call;
    *out = 0;
    return (capture->namelist_count || capture->param_count || capture->content != TA_NIL)
               ? TA_ERROR_EXECUTION
               : TA_OK;
}

/* <log expr="'text'"/>: the null datamodel has no expressions, but a quoted
 * string literal is passed through so logs remain useful. */
static ta_status null_log(const ta_datamodel_call *call, ta_expression expression,
                          ta_string_view *out) {
    ta_datamodel_environment *environment = call->environment;
    ta_expression_info info;
    if (ta_image_expression(
            ta_datamodel_environment_image(environment),
            (ta_chart_item){ta_datamodel_environment_chart(environment), expression},
            &info) != TA_OK)
        return TA_ERROR_EXECUTION;
    ta_string_view s = info.source;
    while (s.length && is_space(s.pointer[0])) {
        s.pointer++;
        s.length--;
    }
    while (s.length && is_space(s.pointer[s.length - 1]))
        s.length--;
    if (s.length < 2 || (s.pointer[0] != '\'' && s.pointer[0] != '"') ||
        s.pointer[s.length - 1] != s.pointer[0])
        return TA_ERROR_EXECUTION;
    out->pointer = s.pointer + 1;
    out->length = s.length - 2;
    return TA_OK;
}

/* No state at all: hibernation stores nothing. */
static ta_status null_serialize(const ta_datamodel_call *call, ta_buffer *out) {
    (void)call;
    out->length = 0;
    return TA_OK;
}

static ta_status null_deserialize(void *datamodel, const ta_datamodel_start *start,
                                  uint64_t *instance) {
    (void)datamodel;
    (void)start;
    *instance = 1;
    return TA_OK;
}

static const ta_datamodel_vtable null_vt = {
    sizeof(ta_datamodel_vtable),
    TA_DATAMODEL_EVENT_DETACHABLE, /* _event is unobservable: stable actors pin no event */
    "null",
    0,                /* chart_attach */
    0,                /* chart_detach */
    null_create,      /* create */
    0,                /* destroy */
    0,                /* init_data */
    0,                /* bind_event */
    0,                /* detach_event */
    null_condition,   /* eval_condition */
    0,                /* eval_string */
    0,                /* assign */
    0,                /* assign_string */
    0,                /* script */
    null_log,         /* log */
    0,                /* foreach_begin */
    0,                /* foreach_item */
    0,                /* foreach_end */
    null_capture,     /* capture */
    0,                /* retain */
    0,                /* release */
    0,                /* custom */
    null_serialize,   /* serialize */
    null_deserialize, /* deserialize */
    0,                /* migrate: nothing to carry over */
    0,                /* to_wire: no data */
    0,                /* from_wire */
    0,                /* payload_serialize: no payloads */
    0,                /* payload_deserialize */
    0,                /* variable: no data */
};

const ta_datamodel_vtable *ta_datamodel_null(void) {
    return &null_vt;
}
