#include "tinyactors_host/params.h"

const tah_param_info tah_param_table[TAH_PARAM_COUNT] = {
    [TAH_PARAM_VALUE_BYTES] = { "valueBytes", "bytes", TAH_PARAM_DEFAULT_VALUE_BYTES, 16, 1048576 },
    [TAH_PARAM_RESUME_BUDGET] = { "resumeBudget", "microsteps", TAH_PARAM_DEFAULT_RESUME_BUDGET, 1, 2147483647 },
    [TAH_PARAM_ACTION_BUDGET] = { "actionBudget", "units", TAH_PARAM_DEFAULT_ACTION_BUDGET, 0, 2147483647 },
    [TAH_PARAM_RESUMES_PER_TURN] = { "resumesPerTurn", "actors", TAH_PARAM_DEFAULT_RESUMES_PER_TURN, 1, 2147483647 },
    [TAH_PARAM_DUE_BUDGET] = { "dueBudget", "sends", TAH_PARAM_DEFAULT_DUE_BUDGET, 1, 2147483647 },
    [TAH_PARAM_MESSAGE_BUDGET] = { "messageBudget", "messages", TAH_PARAM_DEFAULT_MESSAGE_BUDGET, 1, 2147483647 },
    [TAH_PARAM_MAX_QUEUED_MESSAGES] = { "maxQueuedMessages", "messages", TAH_PARAM_DEFAULT_MAX_QUEUED_MESSAGES, 0,
                                        2147483647 },
    [TAH_PARAM_MAX_RETAINED_BYTES] = { "maxRetainedBytes", "bytes", TAH_PARAM_DEFAULT_MAX_RETAINED_BYTES, 0,
                                       9007199254740991u },
};

void tah_params_defaults(tah_params *params) {
    for (uint32_t index = 0; index < TAH_PARAM_COUNT; index += 1u) {
        params->values[index] = tah_param_table[index].default_value;
    }
}
