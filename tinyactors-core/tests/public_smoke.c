/* Embedders see only the public header; nothing under src/ may be required. */
#include "../include/tinyactors.h"

static void use_public_types(void) {
    ta_datamodel datamodel = {0};
    ta_actor_directory directory = {0};
    ta_actor_address address = {0};
    ta_actor_spawn_params spawn = {0};
    ta_actor_unload_params unload = {0};
    ta_actor_load_params load = {0};
    ta_actor_reconstruct_params reconstruct = {0};
    ta_actor_remove_unloaded_params remove_unloaded = {0};
    ta_actor_directory_entry directory_entry = {
        .kind = TA_ACTOR_DIRECTORY_UNLOADED,
    };
    ta_actor_prepare_params prepare = {0};
    ta_actor_publish_params activate = {0};
    ta_actor_completion completion = {0};
    ta_due_dispatch_params due = {0};
    ta_external_event_router router = {0};
    ta_scxml_target target = {0};
    ta_definition_compile_params compile = {0};
    ta_scxml_done_data_spec done_data = {0};
    ta_scxml_action_spec actions[] = {
        {
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = {
                .id = { .kind = TA_SCXML_ID_LITERAL,
                        .as.literal = { .ptr = "timeout", .len = 7 } },
                .type = { .kind = TA_SCXML_STRING_LITERAL,
                          .as.literal = { .ptr = "http", .len = 4 } },
                .event = { .kind = TA_SCXML_EVENT_NAME,
                           .as.name = { .ptr = "tick", .len = 4 } },
                .delay = { .kind = TA_SCXML_STRING_LITERAL,
                           .as.literal = { .ptr = "5s", .len = 2 } },
            },
        },
        {
            .kind = TA_SCXML_ACTION_CANCEL,
            .as.cancel.sendid = { .kind = TA_SCXML_STRING_LITERAL,
                                  .as.literal = { .ptr = "timeout", .len = 7 } },
        },
        {
            .kind = TA_SCXML_ACTION_LOG,
            .as.log.label = { .ptr = "entered", .len = 7 },
        },
        {0},
    };
    ta_scxml_invoke_spec invoke = {
        .kind = TA_SCXML_INVOKE,
        .id = { .kind = TA_SCXML_ID_LOCATION,
                .as.location = { .kind = TA_EXPRESSION_SOURCE,
                                 .as.source = { .ptr = "child", .len = 5 } } },
    };
    ta_scxml_spec spec = { .binding = TA_SCXML_BINDING_LATE };
    ta_invoke_report_params invoke_report = {
        .invoke_id = TA_INVOKE_ID_NONE,
        .kind = TA_INVOKE_REPORT_ERROR,
    };
    ta_session_image *image = 0;
    ta_arena arena = {0};
    ta_queued_event queued = {0};
    ta_active_invoke active = {0};
    ta_actor_inspection inspection = { .residency = TA_ACTOR_IDLE };

    (void)datamodel;
    (void)directory;
    (void)address;
    (void)spawn;
    (void)unload;
    (void)load;
    (void)reconstruct;
    (void)remove_unloaded;
    (void)directory_entry;
    (void)prepare;
    (void)activate;
    (void)completion;
    (void)due;
    (void)router;
    (void)target;
    (void)compile;
    (void)done_data;
    (void)actions;
    (void)invoke;
    (void)spec;
    (void)invoke_report;
    (void)image;
    (void)arena;
    (void)queued;
    (void)active;
    (void)inspection;
}

int main(void) {
    use_public_types();
    return 0;
}
