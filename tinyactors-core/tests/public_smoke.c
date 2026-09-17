#include "../src/datamodel.h"
#include "../src/session_image.h"
#include "../include/tinyactors.h"

static void use_public_types(void) {
    ta_datamodel datamodel = {0};
    ta_actor_directory directory = {0};
    ta_actor_address address = {0};
    ta_actor_spawn_params spawn = {0};
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
    (void)fault_event;
    (void)invoke_report;
    (void)image;
}

int main(void) {
    use_public_types();
    return 0;
}
