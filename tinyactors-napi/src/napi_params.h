#ifndef TNA_NAPI_PARAMS_H
#define TNA_NAPI_PARAMS_H

/*
 * napi's own system parameters (napi ADR 0035), beside core's (core ADR
 * 0092) and the kernel's (tinyactors-host's params.h): the limits of napi's
 * data-model registry. Named, bounded, set once when the system is created;
 * parameterInfo() reports them beside the kernel's, with owner "napi"
 * (the kernel's have owner "kernel").
 */
#include <stdint.h>

typedef enum {
    TNA_PARAM_REGISTERED_DATAMODELS, /* data models registerDatamodel() may add */
    TNA_PARAM_DATAMODEL_NAME_BYTES,  /* longest registered data model name, in bytes */
    TNA_PARAM_COUNT
} tna_param;

enum {
    TNA_PARAM_DEFAULT_REGISTERED_DATAMODELS = 8,
    TNA_PARAM_DEFAULT_DATAMODEL_NAME_BYTES = 32
};

typedef struct {
    const char *name;
    const char *unit;
    uint64_t default_value;
    uint64_t min;
    uint64_t max;
} tna_param_info;

extern const tna_param_info tna_param_table[TNA_PARAM_COUNT];

/* One system's values of napi's parameters, fixed at creation. */
typedef struct {
    uint64_t values[TNA_PARAM_COUNT];
} tna_params;

/* Every parameter at its default. */
void tna_params_defaults(tna_params *params);

#endif
