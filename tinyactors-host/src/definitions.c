#include "tinyactors_host/definitions.h"

void tah_definitions_init(tah_definitions *definitions, ta_allocator allocator) {
    *definitions = (tah_definitions){ .registering = TAH_ORIGIN_HOST };
    tah_map_init(&definitions->origins, allocator);
}

void tah_definitions_destroy(tah_definitions *definitions) {
    tah_map_destroy(&definitions->origins);
}

tah_definition_origin tah_definitions_origin(const tah_definitions *definitions, ta_definition_id definition) {
    uint64_t origin = TAH_ORIGIN_HOST;
    (void)tah_map_get(&definitions->origins, definition, &origin);
    return origin < TAH_ORIGINS ? (tah_definition_origin)origin : TAH_ORIGIN_HOST;
}

tah_definition_origin tah_definitions_note(tah_definitions *definitions, const ta_definition_event *event) {
    switch (event->kind) {
        case TA_DEFINITION_EVENT_REGISTERED: {
            definitions->registered += 1u;
            tah_definition_origin origin = definitions->registering;
            /* The host's are the default: only a child's origin is kept. */
            if (origin != TAH_ORIGIN_HOST && tah_map_put(&definitions->origins, event->definition, origin) != TA_OK) {
                return TAH_ORIGIN_HOST;
            }
            return origin;
        }
        case TA_DEFINITION_EVENT_RETIRED:
            definitions->retired += 1u;
            return tah_definitions_origin(definitions, event->definition);
        case TA_DEFINITION_EVENT_FREED: {
            definitions->freed += 1u;
            tah_definition_origin origin = tah_definitions_origin(definitions, event->definition);
            (void)tah_map_remove(&definitions->origins, event->definition);
            return origin;
        }
    }
    return TAH_ORIGIN_HOST;
}

const char *tah_definition_origin_name(tah_definition_origin origin) {
    switch (origin) {
        case TAH_ORIGIN_NESTED: return "nested";
        case TAH_ORIGIN_INVOKE: return "invoke";
        default: return "host";
    }
}

void tah_definitions_alive(const tah_definitions *definitions, const ta_system *core,
                           uint64_t alive[TAH_ORIGINS][2]) {
    for (uint32_t origin = 0; origin < TAH_ORIGINS; origin += 1u) {
        alive[origin][0] = 0;
        alive[origin][1] = 0;
    }
    ta_definition_id page[256];
    uint32_t from = 0;
    for (;;) {
        uint32_t count = 0;
        ta_result result = ta_system_definitions(core, &(ta_system_definitions_params){
                                                           .from = from,
                                                           .limit = 256,
                                                           .definitions = page,
                                                           .capacity = 256,
                                                           .count = &count,
                                                       });
        if (result != TA_OK) {
            return;
        }
        for (uint32_t at = 0; at < count; at += 1u) {
            ta_definition_inspection view;
            if (ta_definition_inspect(core, page[at], &view) != TA_OK) {
                continue;
            }
            alive[tah_definitions_origin(definitions, page[at])][view.retired ? 1 : 0] += 1u;
        }
        if (count < 256) {
            return;
        }
        from = (uint32_t)(page[count - 1u] & 0xffffffffu) + 1u;
    }
}

void tah_definitions_set_registering(tah_definitions *definitions, tah_definition_origin origin) {
    definitions->registering = origin;
}

tah_definition_totals tah_definitions_totals(const tah_definitions *definitions) {
    return (tah_definition_totals){
        .registered = definitions->registered, .retired = definitions->retired, .freed = definitions->freed };
}
