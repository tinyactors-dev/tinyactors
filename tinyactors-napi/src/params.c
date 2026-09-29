#include "params.h"

const tna_param_info tna_param_table[TNA_PARAM_COUNT] = {
    [TNA_PARAM_TRACE_VALUE_BYTES] = { "traceValueBytes", "bytes", TNA_PARAM_DEFAULT_TRACE_VALUE_BYTES, 16, 1048576 },
    [TNA_PARAM_TRACE_STAGING_BYTES] = { "traceStagingBytes", "bytes", TNA_PARAM_DEFAULT_TRACE_STAGING_BYTES, 65536,
                                        1073741824 },
    [TNA_PARAM_REGISTERED_DATAMODELS] = { "registeredDatamodels", "data models",
                                          TNA_PARAM_DEFAULT_REGISTERED_DATAMODELS, 0, 1024 },
    [TNA_PARAM_DATAMODEL_NAME_BYTES] = { "datamodelNameBytes", "bytes", TNA_PARAM_DEFAULT_DATAMODEL_NAME_BYTES, 8,
                                         256 },
    [TNA_PARAM_RESUME_BUDGET] = { "resumeBudget", "microsteps", TNA_PARAM_DEFAULT_RESUME_BUDGET, 1, 2147483647 },
    [TNA_PARAM_ACTION_BUDGET] = { "actionBudget", "units", TNA_PARAM_DEFAULT_ACTION_BUDGET, 0, 2147483647 },
    [TNA_PARAM_RESUMES_PER_TURN] = { "resumesPerTurn", "actors", TNA_PARAM_DEFAULT_RESUMES_PER_TURN, 1, 2147483647 },
    [TNA_PARAM_DUE_BUDGET] = { "dueBudget", "sends", TNA_PARAM_DEFAULT_DUE_BUDGET, 1, 2147483647 },
    [TNA_PARAM_MESSAGE_BUDGET] = { "messageBudget", "messages", TNA_PARAM_DEFAULT_MESSAGE_BUDGET, 1, 2147483647 },
    [TNA_PARAM_MAX_QUEUED_MESSAGES] = { "maxQueuedMessages", "messages", TNA_PARAM_DEFAULT_MAX_QUEUED_MESSAGES, 0,
                                        2147483647 },
    [TNA_PARAM_MAX_RETAINED_BYTES] = { "maxRetainedBytes", "bytes", TNA_PARAM_DEFAULT_MAX_RETAINED_BYTES, 0,
                                       9007199254740991u },
};

void tna_params_defaults(tna_params *params) {
    for (uint32_t index = 0; index < TNA_PARAM_COUNT; index += 1u) {
        params->values[index] = tna_param_table[index].default_value;
    }
}
