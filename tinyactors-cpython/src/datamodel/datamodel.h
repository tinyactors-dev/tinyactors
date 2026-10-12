/* datamodel.h — the "python" datamodel: chart logic in Python behind the
 * datamodel vtable (tinyactors/datamodel.h; tinyactors-core/docs/
 * bindings.md, section 8). Of core it calls only what a datamodel may:
 * ta_datamodel_environment_*, ta_image_* and ta_cbor_* (AGENTS.md, "C
 * rules": datamodel-calls-environment-and-python).
 *
 *   handles.c      the handle tables: sessions, charts, payloads and loops
 *                  are rows, and core holds a slot and a generation
 *   expressions.c  a chart's expressions, compiled once at chart_attach,
 *                  and their evaluation in a session's one globals dict
 *   datamodel.c    the vtable's entries
 *
 * The datamodel and its tables live in the Host (extension.h): nothing
 * here allocates but the tables (AGENTS.md, "The binding's memory"). A row
 * moves when its table grows, so code holds handles, and the Python
 * objects a row refers to, across anything that can run Python.
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

/* A payload's value, or a <foreach>'s items. */
typedef struct ta_python_value {
    ta_python_row row;
    PyObject *value;
} ta_python_value;

/* A chart attached to the datamodel; its handle is chart_attach's context. */
typedef struct ta_python_chart {
    ta_python_row row;
    const ta_image *image;
    uint32_t chart;
    uint32_t pad;
    PyObject *expressions; /* a list: per expression, (kind, source, code, assign, root) */
} ta_python_chart;

/* An expression's record in a chart's list (expressions.c). */
enum {
    TA_PYTHON_EXPRESSION_KIND = 0,   /* TA_EXPRESSION_*, an int */
    TA_PYTHON_EXPRESSION_SOURCE = 1, /* its text, a str (None: not UTF-8) */
    TA_PYTHON_EXPRESSION_CODE = 2,   /* its code (eval, or exec for a script); None: it does not compile */
    TA_PYTHON_EXPRESSION_ASSIGN = 3, /* a location's `<location> = __ta_value__`; None: none */
    TA_PYTHON_EXPRESSION_ROOT = 4,   /* a location's first name, which must be declared; None: none */
    TA_PYTHON_EXPRESSION_FIELDS = 5
};

/* What a session keeps: its one globals dict and its system variables, as
 * they must stay. */
enum {
    TA_PYTHON_EVENT = 0,
    TA_PYTHON_SESSIONID = 1,
    TA_PYTHON_NAME = 2,
    TA_PYTHON_IOPROCESSORS = 3,
    TA_PYTHON_X = 4,
    TA_PYTHON_IN = 5, /* the datamodel's own names, kept as the system variables are */
    TA_PYTHON_BUILTINS = 6,
    TA_PYTHON_SYSTEM_VARIABLES = 7
};

typedef struct ta_python_session {
    ta_python_row row;
    uint64_t chart;                               /* its chart's handle */
    PyObject *globals;                            /* the session's variables, and no locals */
    PyObject *system[TA_PYTHON_SYSTEM_VARIABLES]; /* NULL: not bound (_event before the first event, _x) */
    PyObject *init;                               /* the spawn's or invoke's data: a dict, or NULL */
} ta_python_session;

/* Drops `object` later: at settle, outside core (NULL: nothing). */
void ta_python_datamodel_defer(ta_python_datamodel *datamodel, PyObject *object);

/* ---- expressions.c ------------------------------------------------------------------------ */

/* The expressions of chart `chart` of `image`, compiled: a new list of
 * records; one that does not compile fails when it is evaluated
 * (error.execution), as SCXML has it. NULL with a Python error. */
typedef struct ta_python_chart_source {
    const ta_image *image;
    uint32_t chart;
} ta_python_chart_source;
PyObject *ta_python_chart_compile(const ta_python_chart_source *source);

/* An evaluation in a session. */
typedef struct ta_python_evaluation {
    ta_python_datamodel *datamodel;
    uint64_t session;
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
/* The record of an expression of a session's chart: a new reference, or
 * NULL. */
PyObject *ta_python_expression_of(const ta_python_evaluation *evaluation, ta_expression expression);

#endif /* TA_PYTHON_DATAMODEL_H */
