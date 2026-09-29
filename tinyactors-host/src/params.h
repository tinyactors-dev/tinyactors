#ifndef TAH_PARAMS_H
#define TAH_PARAMS_H

/*
 * napi's own system parameters (napi ADR 0035), beside core's (core ADR
 * 0092): named, bounded, set once when the system is created, and read
 * back through System.parameters. Defaults are enumerators, so the compiler
 * sees them; tah_param_table is what parameterInfo() reports.
 */
#include <stdint.h>

typedef enum {
    TAH_PARAM_VALUE_BYTES,           /* bytes of a value's JSON in a trace attribute or a log record */
    TAH_PARAM_TRACE_STAGING_BYTES,   /* OTLP staging kept between turns; a turn past it ends early */
    TAH_PARAM_REGISTERED_DATAMODELS, /* data models registerDatamodel() may add */
    TAH_PARAM_DATAMODEL_NAME_BYTES,  /* longest registered data model name, in bytes */
    TAH_PARAM_RESUME_BUDGET,         /* microsteps one resume may run (core's `budget`) */
    TAH_PARAM_ACTION_BUDGET,         /* units of executable content per resume; 0: no limit */
    TAH_PARAM_RESUMES_PER_TURN,      /* runnable actors resumed in one turn */
    TAH_PARAM_DUE_BUDGET,            /* due sends dispatched in one turn */
    TAH_PARAM_MESSAGE_BUDGET,        /* deliveries, and dead letters reported, in one turn */
    TAH_PARAM_MAX_QUEUED_MESSAGES,   /* accepted messages not yet delivered; 0: no limit */
    TAH_PARAM_MAX_RETAINED_BYTES,    /* their retained-bytes estimate; 0: no limit */
    TAH_PARAM_COUNT
} tah_param;

enum {
    TAH_PARAM_DEFAULT_VALUE_BYTES = 1024,
    TAH_PARAM_DEFAULT_TRACE_STAGING_BYTES = 1048576,
    TAH_PARAM_DEFAULT_REGISTERED_DATAMODELS = 8,
    TAH_PARAM_DEFAULT_DATAMODEL_NAME_BYTES = 32,
    TAH_PARAM_DEFAULT_RESUME_BUDGET = 64,
    TAH_PARAM_DEFAULT_ACTION_BUDGET = 1024,
    TAH_PARAM_DEFAULT_RESUMES_PER_TURN = 1024,
    TAH_PARAM_DEFAULT_DUE_BUDGET = 1024,
    TAH_PARAM_DEFAULT_MESSAGE_BUDGET = 1024,
    TAH_PARAM_DEFAULT_MAX_QUEUED_MESSAGES = 0,
    TAH_PARAM_DEFAULT_MAX_RETAINED_BYTES = 0
};

typedef struct {
    const char *name;
    const char *unit;
    uint64_t default_value;
    uint64_t min;
    uint64_t max;
} tah_param_info;

extern const tah_param_info tah_param_table[TAH_PARAM_COUNT];

/* One system's values of napi's parameters, fixed at creation. */
typedef struct {
    uint64_t values[TAH_PARAM_COUNT];
} tah_params;

/* Every parameter at its default. */
void tah_params_defaults(tah_params *params);

#endif
