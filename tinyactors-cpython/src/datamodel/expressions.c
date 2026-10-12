/* expressions.c — a chart's expressions, compiled once at chart_attach,
 * by kind (tinyactors-core/docs/bindings.md, section 8): a condition, a
 * value, a location or a *expr string is a Python expression, a <script>
 * Python statements, and <content> JSON if it parses, else its text with
 * its whitespace runs made one space (as the ECMAScript datamodel reads
 * it, SCXML B.2, without the XML DOM). An expression that does not compile
 * raises error.execution when it is evaluated.
 *
 * Every evaluation runs in the session's one globals dict, with no
 * separate locals, so generator expressions and lambdas see the session's
 * variables (AGENTS.md, "Rules the suite cannot see"). After it, the
 * system variables are checked: one the evaluation changed is restored,
 * and the evaluation is error.execution (bindings.md, section 8: "Where
 * the language's scope cannot refuse it, check after the evaluation and
 * restore"). */
#include "datamodel/datamodel.h"

/* The system variables (SCXML 5.10), and the datamodel's own In and
 * __builtins__: no assignment may name them, and an evaluation that
 * changes one is undone. */
static const char *const ta_python_system_names[TA_PYTHON_SYSTEM_VARIABLES] = {
    "_event", "_sessionid", "_name", "_ioprocessors", "_x", "In", "__builtins__"};

/* Fuel an interrupted evaluation charges: more than a turn's budget
 * (loop.c, TA_PYTHON_BUDGET), small enough that core's counts do not
 * overflow. */
#define TA_PYTHON_INTERRUPT_FUEL (1u << 24)

/* The name assignments go through, then leave the globals again. */
#define TA_PYTHON_VALUE_NAME "__ta_value__"

/* ---- compiling ---------------------------------------------------------------------------- */

/* The first name of a location: its leading identifier characters. */
static PyObject *ta_python_root_of(ta_string_view source) {
    uint32_t start = 0, end;
    while (start < source.length && (source.pointer[start] == ' ' || source.pointer[start] == '\t' ||
                                     source.pointer[start] == '\n' || source.pointer[start] == '\r'))
        start++;
    for (end = start; end < source.length; end++) {
        unsigned char c = (unsigned char)source.pointer[end];
        int letter = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
        if (!letter && !(end > start && c >= '0' && c <= '9')) break;
    }
    if (end == start) return NULL;
    return PyUnicode_DecodeUTF8(source.pointer + start, (Py_ssize_t)(end - start), "strict");
}

/* `text` compiled with `mode` (Py_eval_input, Py_file_input), or NULL
 * with no error: it fails when it is evaluated. */
typedef struct ta_python_compiling {
    PyObject *text;
    const char *filename;
    int mode;
} ta_python_compiling;

static PyObject *ta_python_compile_text(const ta_python_compiling *compiling) {
    const char *utf8 = compiling->text ? PyUnicode_AsUTF8AndSize(compiling->text, NULL) : NULL;
    PyObject *code = utf8 ? Py_CompileString(utf8, compiling->filename, compiling->mode) : NULL;
    if (!code) PyErr_Clear();
    return code;
}

/* A script's text without the indentation its lines share (the chart's
 * XML indents it). */
static PyObject *ta_python_dedent(PyObject *text) {
    PyObject *textwrap = PyImport_ImportModule("textwrap");
    PyObject *dedented = textwrap ? PyObject_CallMethod(textwrap, "dedent", "O", text) : NULL;
    Py_XDECREF(textwrap);
    return dedented;
}

static ta_status ta_python_expression_compile(ta_python_expression *expression, const ta_expression_info *info,
                                              const char *filename) {
    expression->kind = info->kind;
    expression->source = PyUnicode_DecodeUTF8(info->source.pointer, (Py_ssize_t)info->source.length, "strict");
    if (!expression->source) {
        PyErr_Clear();
        return TA_OK; /* evaluates to error.execution */
    }
    switch (info->kind) {
    case TA_EXPRESSION_CONTENT: return TA_OK;
    case TA_EXPRESSION_SCRIPT: {
        PyObject *text = ta_python_dedent(expression->source);
        if (!text) PyErr_Clear();
        expression->code = ta_python_compile_text(&(ta_python_compiling){text, filename, Py_file_input});
        Py_XDECREF(text);
        return TA_OK;
    }
    case TA_EXPRESSION_LOCATION: {
        PyObject *stripped = PyObject_CallMethod(expression->source, "strip", NULL);
        PyObject *assignment = stripped ? PyUnicode_FromFormat("%U = " TA_PYTHON_VALUE_NAME, stripped) : NULL;
        expression->assign = ta_python_compile_text(&(ta_python_compiling){assignment, filename, Py_file_input});
        expression->root = ta_python_root_of(info->source);
        if (!expression->root) PyErr_Clear();
        Py_XDECREF(assignment);
        Py_XDECREF(stripped);
        if (PyErr_Occurred()) PyErr_Clear();
    } /* fall through: a location is read as an expression too (namelist) */
    default: {
        PyObject *stripped = PyObject_CallMethod(expression->source, "strip", NULL);
        expression->code = ta_python_compile_text(&(ta_python_compiling){stripped, filename, Py_eval_input});
        Py_XDECREF(stripped);
        if (PyErr_Occurred()) PyErr_Clear();
        return TA_OK;
    }
    }
}

ta_status ta_python_chart_compile(ta_python_datamodel *datamodel, ta_python_chart *chart) {
    (void)datamodel;
    chart->count = ta_image_expression_count(chart->image, chart->chart);
    chart->expressions = PyMem_RawMalloc((chart->count ? chart->count : 1) * sizeof *chart->expressions);
    if (!chart->expressions) return TA_ERROR_NO_MEMORY;
    memset(chart->expressions, 0, (chart->count ? chart->count : 1) * sizeof *chart->expressions);
    ta_string_view source = ta_image_chart_source(chart->image, chart->chart);
    PyObject *filename = PyUnicode_DecodeUTF8(source.pointer, (Py_ssize_t)source.length, "replace");
    const char *name = filename ? PyUnicode_AsUTF8AndSize(filename, NULL) : "<chart>";
    if (!name) {
        PyErr_Clear();
        name = "<chart>";
    }
    for (uint32_t i = 0; i < chart->count; i++) {
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, i}, &info) != TA_OK) continue;
        ta_python_expression_compile(&chart->expressions[i], &info, name);
    }
    Py_XDECREF(filename);
    return TA_OK;
}

void ta_python_chart_free(ta_python_chart *chart) {
    for (uint32_t i = 0; i < chart->count; i++) {
        Py_CLEAR(chart->expressions[i].source);
        Py_CLEAR(chart->expressions[i].code);
        Py_CLEAR(chart->expressions[i].assign);
        Py_CLEAR(chart->expressions[i].root);
    }
    PyMem_RawFree(chart->expressions);
    chart->expressions = NULL;
    chart->count = 0;
}

PyObject *ta_python_expression_source(const ta_python_session *session, ta_expression expression) {
    if (expression >= session->chart->count) return NULL;
    return session->chart->expressions[expression].source;
}

/* ---- evaluating ---------------------------------------------------------------------------- */

/* What an evaluation raised: error.execution for the chart. A
 * BaseException that is not an Exception (KeyboardInterrupt, SystemExit)
 * is the runtime's: kept, the first one, and raised again after the run
 * (bindings.md, section 8). The exception is dropped later, outside core:
 * its traceback holds frames. */
static ta_status ta_python_raised(ta_python_datamodel *datamodel) {
    PyObject *exception = PyErr_GetRaisedException();
    if (exception && !PyErr_GivenExceptionMatches(exception, PyExc_Exception) && !datamodel->kept)
        datamodel->kept = exception;
    else ta_python_datamodel_defer(datamodel, exception);
    return TA_ERROR_EXECUTION;
}

/* The system variables after an evaluation: each as the session keeps it,
 * else restored, and the evaluation fails. */
static ta_status ta_python_guard(const ta_python_evaluation *evaluation) {
    ta_python_session *session = evaluation->session;
    ta_status status = TA_OK;
    for (int i = 0; i < TA_PYTHON_SYSTEM_VARIABLES; i++) {
        PyObject *now = NULL;
        if (PyDict_GetItemStringRef(session->globals, ta_python_system_names[i], &now) < 0) PyErr_Clear();
        if (now != session->system[i]) {
            status = TA_ERROR_EXECUTION;
            if (session->system[i]) PyDict_SetItemString(session->globals, ta_python_system_names[i], session->system[i]);
            else PyDict_DelItemString(session->globals, ta_python_system_names[i]);
            if (PyErr_Occurred()) PyErr_Clear();
            ta_python_datamodel_defer(evaluation->datamodel, now);
        } else {
            Py_XDECREF(now);
        }
    }
    return status;
}

/* `code` run in the session's globals. */
static PyObject *ta_python_run(const ta_python_evaluation *evaluation, PyObject *code, ta_status *status) {
    ta_python_datamodel *datamodel = evaluation->datamodel;
    if (!code) {
        *status = TA_ERROR_EXECUTION;
        return NULL;
    }
    if (evaluation->environment) ta_datamodel_environment_charge(evaluation->environment, 1);
    ta_datamodel_environment *outer = datamodel->environment;
    ta_python_session *current = datamodel->current;
    datamodel->environment = evaluation->environment;
    datamodel->current = evaluation->session;
    PyObject *result = PyEval_EvalCode(code, evaluation->session->globals, evaluation->session->globals);
    datamodel->environment = outer;
    datamodel->current = current;
    int interrupted = datamodel->kept != NULL;
    *status = result ? TA_OK : ta_python_raised(datamodel);
    /* A KeyboardInterrupt or SystemExit stops the run: the evaluation that
     * raised it charges more fuel than a turn's budget, so the run ends
     * when its microstep does (core.c, burn; schedule.c, run_actor), and
     * the turn raises it (loop.c). The chart sees error.execution and goes
     * on at the next turn. */
    if (!interrupted && datamodel->kept && evaluation->environment)
        ta_datamodel_environment_charge(evaluation->environment, TA_PYTHON_INTERRUPT_FUEL);
    ta_status guarded = ta_python_guard(evaluation);
    if (guarded != TA_OK) {
        *status = guarded;
        ta_python_datamodel_defer(datamodel, result);
        return NULL;
    }
    return result;
}

/* <content>: JSON if it parses (JSON's: NaN and Infinity are not), else
 * the text, its runs of space, tab, CR and LF one space, and trimmed: the
 * ECMAScript datamodel's content_value (datamodel_ecmascript.c:340), but
 * for its XML DOM. */
static PyObject *ta_python_content(ta_python_datamodel *datamodel, PyObject *source) {
    PyObject *arguments = PyTuple_Pack(1, source);
    PyObject *keywords = Py_BuildValue("{sO}", "parse_constant", datamodel->refuse_constant);
    PyObject *value = arguments && keywords ? PyObject_Call(datamodel->json_loads, arguments, keywords) : NULL;
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
    if (value) return value;
    PyErr_Clear();
    Py_ssize_t length = 0;
    const char *text = PyUnicode_AsUTF8AndSize(source, &length);
    char *buffer = text ? PyMem_Malloc((size_t)length + 1) : NULL;
    if (!buffer) return text ? PyErr_NoMemory() : NULL;
    Py_ssize_t written = 0;
    for (Py_ssize_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (written && buffer[written - 1] != ' ') buffer[written++] = ' ';
        } else {
            buffer[written++] = c;
        }
    }
    while (written && buffer[written - 1] == ' ')
        written--;
    value = PyUnicode_DecodeUTF8(buffer, written, "strict");
    PyMem_Free(buffer);
    return value;
}

PyObject *ta_python_evaluate(const ta_python_evaluation *evaluation, ta_expression expression, ta_status *status) {
    ta_python_session *session = evaluation->session;
    *status = TA_ERROR_EXECUTION;
    if (expression >= session->chart->count) return NULL;
    const ta_python_expression *compiled = &session->chart->expressions[expression];
    if (compiled->kind == TA_EXPRESSION_CONTENT) {
        PyObject *value = compiled->source ? ta_python_content(evaluation->datamodel, compiled->source) : NULL;
        *status = value ? TA_OK : ta_python_raised(evaluation->datamodel);
        return value;
    }
    if (compiled->kind == TA_EXPRESSION_SCRIPT) return NULL;
    return ta_python_run(evaluation, compiled->code, status);
}

ta_status ta_python_execute(const ta_python_evaluation *evaluation, ta_expression expression) {
    ta_python_session *session = evaluation->session;
    if (expression >= session->chart->count) return TA_ERROR_EXECUTION;
    ta_status status;
    PyObject *result = ta_python_run(evaluation, session->chart->expressions[expression].code, &status);
    Py_XDECREF(result); /* None */
    return status;
}

static int ta_python_is_system_name(PyObject *name) {
    for (int i = 0; i < TA_PYTHON_SYSTEM_VARIABLES; i++)
        if (PyUnicode_CompareWithASCIIString(name, ta_python_system_names[i]) == 0) return 1;
    return 0;
}

ta_status ta_python_assign_to(const ta_python_evaluation *evaluation, const ta_python_assignment *assignment) {
    ta_python_session *session = evaluation->session;
    if (assignment->location >= session->chart->count) return TA_ERROR_EXECUTION;
    const ta_python_expression *location = &session->chart->expressions[assignment->location];
    if (!location->root || !location->assign || ta_python_is_system_name(location->root)) return TA_ERROR_EXECUTION;
    int declared = PyDict_Contains(session->globals, location->root);
    PyObject *stripped = PyObject_CallMethod(location->source, "strip", NULL);
    int plain = stripped && PyUnicode_Compare(stripped, location->root) == 0;
    Py_XDECREF(stripped);
    if (PyErr_Occurred()) PyErr_Clear();
    if (declared < 0 || (!declared && !(plain && assignment->declare))) return TA_ERROR_EXECUTION; /* undeclared */
    if (plain) /* a name: set it, with no code to run */
        return PyDict_SetItem(session->globals, location->root, assignment->value) < 0 ? ta_python_raised(evaluation->datamodel)
                                                                                       : TA_OK;
    if (PyDict_SetItemString(session->globals, TA_PYTHON_VALUE_NAME, assignment->value) < 0)
        return ta_python_raised(evaluation->datamodel);
    ta_status status;
    PyObject *result = ta_python_run(evaluation, location->assign, &status);
    Py_XDECREF(result);
    if (PyDict_DelItemString(session->globals, TA_PYTHON_VALUE_NAME) < 0) PyErr_Clear();
    return status;
}
