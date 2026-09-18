#include "../src/internal.h"

static void use_types(void) {
    ta_event event = {0};
    ta_fault fault = {0};
    ta_configuration_layout layout = {0};
    ta_delayed_send timer = {0};
    ta_continuation continuation = {0};
    ta_fault_event_params fault_event = {
        .context = TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE,
        .invoke_id = TA_INVOKE_ID_NONE,
    };

    (void)event;
    (void)fault;
    (void)layout;
    (void)timer;
    (void)continuation;
    (void)fault_event;
}

int main(void) {
    use_types();
    return 0;
}
