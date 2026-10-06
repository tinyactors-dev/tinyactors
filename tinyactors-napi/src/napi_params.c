#include "napi_params.h"

const tna_param_info tna_param_table[TNA_PARAM_COUNT] = {
    [TNA_PARAM_REGISTERED_DATAMODELS] = { "registeredDatamodels", "data models",
                                          TNA_PARAM_DEFAULT_REGISTERED_DATAMODELS, 0, 1024 },
    [TNA_PARAM_DATAMODEL_NAME_BYTES] = { "datamodelNameBytes", "bytes", TNA_PARAM_DEFAULT_DATAMODEL_NAME_BYTES, 8,
                                         256 },
    [TNA_PARAM_TRACE_STAGING_BYTES] = { "traceStagingBytes", "bytes", TAO_STAGING_BYTES_DEFAULT, TAO_STAGING_BYTES_MIN,
                                        TAO_STAGING_BYTES_MAX },
};

void tna_params_defaults(tna_params *params) {
    for (uint32_t index = 0; index < TNA_PARAM_COUNT; index += 1u) {
        params->values[index] = tna_param_table[index].default_value;
    }
}
