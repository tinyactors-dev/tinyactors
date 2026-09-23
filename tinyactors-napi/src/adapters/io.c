#include "io.h"

#include "../system.h"

static ta_op_status tna_io_send(void *self, const ta_send *send, ta_fault *fault) {
    tna_system *system = self;
    tna_effect effect = {
        .kind = TNA_EFFECT_IO,
        .source = send->source,
        .send_id = send->send_id,
        .event = send->event,
        .type = send->io_type,
        .value = send->data,
        .target = send->target,
        .content_type = send->content_type,
        .content = send->content,
    };
    if (effect.source.kind == TA_ACTOR_REF_NAMED) {
        ta_address_retain(effect.source.as.named);
    }
    ta_value_retain(&effect.value);
    ta_value_retain(&effect.target);
    if (!tna_system_effect(system, &effect)) {
        static const char message[] = "the I/O processor could not queue the send";
        *fault = (ta_fault){ .code = TA_ERR_NOMEM, .message = { .ptr = message, .len = sizeof message - 1u } };
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

static const ta_io_vtable tna_io_vtable = { .send = tna_io_send };

ta_io tna_io_adapter(tna_system *system) {
    return (ta_io){ .self = system, .v = &tna_io_vtable };
}
