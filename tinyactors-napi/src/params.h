#ifndef TNA_PARAMS_H
#define TNA_PARAMS_H

/*
 * napi's own system parameters (napi ADR 0035), beside core's (core ADR
 * 0092): named, bounded, set once when the system is created, and read
 * back through System.parameters. Defaults are enumerators, so the compiler
 * sees them; tna_param_table is what parameterInfo() reports.
 */
#include <stdint.h>

typedef enum {
    TNA_PARAM_TRACE_VALUE_BYTES,     /* bytes of a value's JSON in a trace or log attribute */
    TNA_PARAM_TRACE_STAGING_BYTES,   /* OTLP staging kept between turns; a turn past it ends early */
    TNA_PARAM_REGISTERED_DATAMODELS, /* data models registerDatamodel() may add */
    TNA_PARAM_DATAMODEL_NAME_BYTES,  /* longest registered data model name, in bytes */
    TNA_PARAM_RESUME_BUDGET,         /* microsteps one resume may run (core's `budget`) */
    TNA_PARAM_ACTION_BUDGET,         /* units of executable content per resume; 0: no limit */
    TNA_PARAM_RESUMES_PER_TURN,      /* runnable actors resumed in one turn */
    TNA_PARAM_DUE_BUDGET,            /* due sends dispatched in one turn */
    TNA_PARAM_MESSAGE_BUDGET,        /* deliveries, and dead letters reported, in one turn */
    TNA_PARAM_MAX_QUEUED_MESSAGES,   /* accepted messages not yet delivered; 0: no limit */
    TNA_PARAM_MAX_RETAINED_BYTES,    /* their retained-bytes estimate; 0: no limit */
    TNA_PARAM_COUNT
} tna_param;

enum {
    TNA_PARAM_DEFAULT_TRACE_VALUE_BYTES = 1024,
    TNA_PARAM_DEFAULT_TRACE_STAGING_BYTES = 1048576,
    TNA_PARAM_DEFAULT_REGISTERED_DATAMODELS = 8,
    TNA_PARAM_DEFAULT_DATAMODEL_NAME_BYTES = 32,
    TNA_PARAM_DEFAULT_RESUME_BUDGET = 64,
    TNA_PARAM_DEFAULT_ACTION_BUDGET = 1024,
    TNA_PARAM_DEFAULT_RESUMES_PER_TURN = 1024,
    TNA_PARAM_DEFAULT_DUE_BUDGET = 1024,
    TNA_PARAM_DEFAULT_MESSAGE_BUDGET = 1024,
    TNA_PARAM_DEFAULT_MAX_QUEUED_MESSAGES = 0,
    TNA_PARAM_DEFAULT_MAX_RETAINED_BYTES = 0
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
