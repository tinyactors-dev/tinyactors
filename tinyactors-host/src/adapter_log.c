#include "tinyactors_host/adapter_log.h"

#include "tinyactors_host/system.h"
#include "system_private.h"

static void tah_log_write(void *self, const ta_log_record *record) {
    tah_system *system = self;
    if (system->otlp != NULL) {
        /* The trace records written before it go first, so logs keep core's order. */
        tah_system_drain_telemetry(system);
        tao_log(system->otlp, record); /* an OTLP log record (napi ADR 0017 section 7) */
    }
    if (!system->log_to_js) {
        return;
    }
    tah_effect effect = {
        .kind = TAH_EFFECT_LOG,
        .source = record->source,
        .label = record->label,
        .has_value = record->has_value,
    };
    if (effect.source.kind == TA_ACTOR_REF_NAMED) {
        ta_address_retain(effect.source.as.named);
    }
    if (record->has_value) {
        effect.value = record->value;
        ta_value_retain(&effect.value);
    }
    (void)tah_system_effect(system, &effect); /* a <log> cannot fail (SCXML 4.7) */
}

static const ta_log_sink_vtable tah_log_vtable = { .write = tah_log_write };

ta_log_sink tah_log_sink_adapter(tah_system *system) {
    return (ta_log_sink){ .self = system, .v = &tah_log_vtable };
}
