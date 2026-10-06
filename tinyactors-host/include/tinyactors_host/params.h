#ifndef TAH_PARAMS_H
#define TAH_PARAMS_H

/*
 * The kernel's system parameters (napi ADR 0035, kernel ADR 0001 step 2b),
 * beside core's (core ADR 0092): how a system is hosted (turn budgets,
 * mailbox limits, trace sizes), named, bounded, set once when the system is
 * created. Defaults are enumerators, so the compiler sees them. A binding
 * reports them with its own (napi's parameterInfo() lists them as napi's,
 * since napi is what embeds the kernel).
 */
#include <stdint.h>

typedef enum {
    TAH_PARAM_VALUE_BYTES,           /* bytes of a value's JSON in a trace attribute or a log record; core's
                                        telemetryValueBytes follows it unless set (core ADR 0098) */
    TAH_PARAM_RESUME_BUDGET,         /* microsteps one ta_run() call may run (core's `budget`); core's
                                        telBytes holds one call's records unless set (core ADR 0098) */
    TAH_PARAM_ACTION_BUDGET,         /* units of executable content per ta_run() call; 0: no limit */
    TAH_PARAM_RESUMES_PER_TURN,      /* ta_run() calls in one turn */
    TAH_PARAM_DUE_BUDGET,            /* due sends dispatched in one turn */
    TAH_PARAM_MESSAGE_BUDGET,        /* actors whose held mail is admitted, and dead letters reported, in one turn */
    TAH_PARAM_MAX_QUEUED_MESSAGES,   /* accepted messages not yet taken past which a message marked
                                        may_overflow waits as overflow (kernel ADR 0003); mail between
                                        actors stays in core (core ADR 0099); 0: no limit */
    TAH_PARAM_MAX_RETAINED_BYTES,    /* their retained-bytes estimate, likewise; 0: no limit */
    TAH_PARAM_COUNT
} tah_param;

enum {
    TAH_PARAM_DEFAULT_VALUE_BYTES = 1024,
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

/* One system's values of the kernel's parameters, fixed at creation. */
typedef struct {
    uint64_t values[TAH_PARAM_COUNT];
} tah_params;

/* Every parameter at its default. */
void tah_params_defaults(tah_params *params);

#endif
