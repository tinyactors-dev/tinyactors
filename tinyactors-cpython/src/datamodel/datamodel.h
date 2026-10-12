/* datamodel.h — the "python" datamodel: chart logic in Python behind the
 * datamodel vtable (tinyactors/datamodel.h; tinyactors-core/docs/
 * bindings.md, section 8). Of core it calls only what a datamodel may:
 * ta_datamodel_environment_*, ta_image_* and ta_cbor_* (AGENTS.md, "C
 * rules": datamodel-calls-environment-and-python).
 *
 *   handles.c      the two slot tables, sessions and payloads: a slot and a
 *                  generation per handle, never a pointer
 *   expressions.c  a chart's expressions, compiled once at chart_attach,
 *                  and their evaluation in a session's one globals dict
 *   datamodel.c    the vtable's entries
 *
 * Rules the suite cannot see (AGENTS.md), each with a test of its own:
 * the last drop of a Python object is deferred until the outer call into
 * core returns; system variables are restored after an evaluation that
 * changed them, which is error.execution; a BaseException that is not an
 * Exception is error.execution for the chart and raised again after the
 * run. */
#ifndef TA_PYTHON_DATAMODEL_H
#define TA_PYTHON_DATAMODEL_H

#include "extension.h"

/* ---- handles.c ------------------------------------------------------------------------- */

/* A slot table: each handle is (generation << 32) | (slot + 1), so 0 is
 * no handle and a handle whose slot was reused is refused (stale). */
typedef struct ta_python_slot {
    void *item;          /* NULL: free */
    uint32_t generation; /* bumped when the slot is freed */
    uint32_t count;      /* payloads: references; free slots: the next free slot + 1 */
} ta_python_slot;

typedef struct ta_python_handles {
    ta_python_slot *slots;
    uint32_t count, capacity;
    uint32_t free; /* the first free slot + 1, 0: none */
} ta_python_handles;

/* A new handle for `item`, with one reference; 0 when memory ran out. */
uint64_t ta_python_handle_add(ta_python_handles *table, void *item);
/* The slot of `handle`, or NULL for a handle that is not (any more). */
ta_python_slot *ta_python_handle_slot(const ta_python_handles *table, uint64_t handle);
/* Frees the slot of `handle` and returns its item (NULL: stale). */
void *ta_python_handle_remove(ta_python_handles *table, uint64_t handle);
void ta_python_handles_free(ta_python_handles *table);

/* ---- the datamodel ------------------------------------------------------------------------ */

/* An expression of a chart, compiled once (expressions.c). */
typedef struct ta_python_expression {
    uint32_t kind;      /* TA_EXPRESSION_* */
    PyObject *source;   /* str: the expression's text */
    PyObject *code;     /* its code (eval, or exec for a script); NULL: it does not compile */
    PyObject *assign;   /* a location's assignment, `<location> = __ta_value__`; NULL: none */
    PyObject *root;     /* a location's first name (str), which must be declared; NULL: none */
} ta_python_expression;

/* A chart attached to the datamodel (chart_attach's context). */
typedef struct ta_python_chart {
    const ta_image *image;
    uint32_t chart;
    uint32_t count; /* expressions */
    ta_python_expression *expressions;
} ta_python_chart;

/* What a session keeps: its one globals dict and its system variables, as
 * they must stay. */
enum {
    TA_PYTHON_EVENT = 0,
    TA_PYTHON_SESSIONID = 1,
    TA_PYTHON_NAME = 2,
    TA_PYTHON_IOPROCESSORS = 3,
    TA_PYTHON_X = 4,
    TA_PYTHON_IN = 5,       /* the datamodel's own names, kept as the system variables are */
    TA_PYTHON_BUILTINS = 6,
    TA_PYTHON_SYSTEM_VARIABLES = 7
};

typedef struct ta_python_session {
    ta_python_chart *chart;
    PyObject *globals;                                   /* the session's variables, and no locals */
    PyObject *system[TA_PYTHON_SYSTEM_VARIABLES];        /* NULL: not bound (_event before the first event, _x) */
    PyObject *init;                                      /* the spawn's or invoke's data: a dict, or NULL */
} ta_python_session;

typedef struct ta_python_datamodel {
    const ta_python_state *state;
    ta_python_handles sessions; /* items: ta_python_session * */
    ta_python_handles payloads; /* items: PyObject *, count its references */
    ta_python_handles loops;    /* items: PyObject * (a list), one per <foreach> in progress */
    PyObject *builtins;         /* the builtins module's dict, each session's __builtins__ */
    PyObject *json_loads;       /* json.loads, for <content> */
    PyObject *refuse_constant;  /* json.loads's parse_constant, int: NaN and Infinity are not JSON */
    PyObject *in;               /* In(state), bound to this datamodel */
    PyObject *deferred;         /* a list: objects whose last reference waits for the outer call to return */
    PyObject *ended;            /* a list: the globals of ended sessions, cleared at settle (they make cycles) */
    PyObject *kept;             /* the first BaseException that is not an Exception, raised after the run */
    PyObject *text;             /* the str whose UTF-8 eval_string and log returned last */
    ta_datamodel_environment *environment; /* of the call in progress, for In() */
    ta_python_session *current;            /* the session of the call in progress */
} ta_python_datamodel;

/* The vtable, name "python". */
const ta_datamodel_vtable *ta_python_datamodel_vtable(void);
/* A datamodel for a host; NULL with a Python error. */
ta_python_datamodel *ta_python_datamodel_new(const ta_python_state *state);
/* After core freed every session and payload (ta_host_free). */
void ta_python_datamodel_free(ta_python_datamodel *datamodel);
/* After an outer call into core returned: the deferred drops happen, and
 * a kept BaseException is set as the Python error (-1). */
int ta_python_datamodel_settle(ta_python_datamodel *datamodel);
/* A BaseException is kept: the run in progress is to stop. */
int ta_python_datamodel_interrupted(const ta_python_datamodel *datamodel);
/* Drops `object` later: at settle, outside core (NULL: nothing). */
void ta_python_datamodel_defer(ta_python_datamodel *datamodel, PyObject *object);

/* ---- expressions.c ------------------------------------------------------------------------ */

/* Compiles every expression of `chart`; one that does not compile fails
 * when it is evaluated (error.execution), as SCXML has it. */
ta_status ta_python_chart_compile(ta_python_datamodel *datamodel, ta_python_chart *chart);
void ta_python_chart_free(ta_python_chart *chart);

/* An evaluation in a session. */
typedef struct ta_python_evaluation {
    ta_python_datamodel *datamodel;
    ta_python_session *session;
    ta_datamodel_environment *environment; /* NULL outside a step */
} ta_python_evaluation;

/* The value of expression `expression` (a new reference), or NULL with
 * the status in *status: TA_ERROR_EXECUTION when it raised or changed a
 * system variable. */
PyObject *ta_python_evaluate(const ta_python_evaluation *evaluation, ta_expression expression, ta_status *status);
/* Runs a script. */
ta_status ta_python_execute(const ta_python_evaluation *evaluation, ta_expression expression);
/* Assigns `value` to location `location`; `declare`: a plain name that is
 * not declared yet is declared (a <foreach> item), else it must be. */
typedef struct ta_python_assignment {
    ta_expression location;
    PyObject *value;
    int declare;
} ta_python_assignment;
ta_status ta_python_assign_to(const ta_python_evaluation *evaluation, const ta_python_assignment *assignment);
/* The source text of an expression (a borrowed str). */
PyObject *ta_python_expression_source(const ta_python_session *session, ta_expression expression);

#endif /* TA_PYTHON_DATAMODEL_H */
