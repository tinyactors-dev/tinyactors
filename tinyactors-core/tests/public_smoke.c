#include "../src/datamodel.h"
#include "../src/session_image.h"
#include "../include/tinyactors.h"

static void use_public_types(void) {
    ta_datamodel datamodel = {0};
    ta_actor_directory directory = {0};
    ta_actor_address address = {0};
    ta_actor_spawn_params spawn = {0};
    ta_actor_unload_params unload = {0};
    ta_actor_load_params load = {0};
    ta_actor_remove_unloaded_params remove_unloaded = {0};
    ta_actor_directory_entry directory_entry = {
        .kind = TA_ACTOR_DIRECTORY_UNLOADED,
    };
    ta_actor_prepare_replacement_params prepare = {0};
    ta_actor_activate_replacement_params activate = {0};
    ta_actor_completion completion = {0};
    ta_due_dispatch_params due = {0};
    ta_external_event_router router = {0};
    ta_scxml_target target = {0};
    ta_definition_compile_params compile = {0};
    ta_scxml_done_data_spec done_data = {0};
    ta_fault_event_params fault_event = {
        .context = TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE,
        .invoke_id = TA_INVOKE_ID_NONE,
    };
    ta_invoke_report_params invoke_report = {
        .invoke_id = TA_INVOKE_ID_NONE,
        .kind = TA_INVOKE_REPORT_UNAVAILABLE,
    };
    ta_session_image *image = 0;

    (void)datamodel;
    (void)directory;
    (void)address;
    (void)spawn;
    (void)unload;
    (void)load;
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
    (void)fault_event;
    (void)invoke_report;
    (void)image;
}

int main(void) {
    use_public_types();
    return 0;
}
