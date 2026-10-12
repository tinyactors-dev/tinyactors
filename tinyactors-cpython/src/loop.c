/* loop.c — the queue loop (tinyactors-core/docs/bindings.md, section 3):
 * one turn feeds the clocks, runs the host until it is idle with every run
 * status handled, and dispatches and acknowledges the effects; and the
 * time the turn runs at, from the real clock or a SimulatedClock.
 *
 * The one timer lives here (AGENTS.md, "C rules": one-timer-in-loop): the
 * synchronous driver, Host.run, sleeps to the next deadline with
 * `time.sleep`, so Ctrl-C interrupts it as any sleep. */
#include "extension.h"

/* Fuel per ta_host_run: a turn runs again while the host yields, so this
 * bounds a run, not a turn (bindings.md, section 3, "Bound each turn"). */
#define TA_PYTHON_BUDGET 100000u

/* ---- SimulatedClock ----------------------------------------------------------------------- */

/* tinyactors.SimulatedClock: the time of the hosts made with it, moved
 * only by increment(ms) (xstate's shape, our meaning: ROADMAP.md, M1). Its
 * hosts are weak references: a clock keeps no host alive. */
typedef struct ta_python_clock {
    PyObject_HEAD
    uint64_t milliseconds; /* the simulated time, from 0 */
    PyObject *hosts;       /* a list of weak references */
} ta_python_clock;

static PyObject *ta_python_clock_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, ":SimulatedClock", (char **)keywords)) return NULL;
    ta_python_clock *clock = (ta_python_clock *)PyType_GenericAlloc(type, 0);
    if (!clock) return NULL;
    clock->hosts = PyList_New(0);
    if (!clock->hosts) {
        Py_DECREF(clock);
        return NULL;
    }
    return (PyObject *)clock;
}

static int ta_python_clock_traverse(PyObject *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(((ta_python_clock *)self)->hosts);
    return 0;
}

static int ta_python_clock_clear(PyObject *self) {
    Py_CLEAR(((ta_python_clock *)self)->hosts);
    return 0;
}

static void ta_python_clock_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_clock_clear(self);
    PyObject_GC_Del(self);
    Py_DECREF(type);
}

int ta_python_clock_attach(PyObject *clock, ta_python_host *host) {
    PyObject *reference = PyWeakref_NewRef((PyObject *)host, NULL);
    if (!reference) return -1;
    int added = PyList_Append(((ta_python_clock *)clock)->hosts, reference);
    Py_DECREF(reference);
    if (added < 0) return -1;
    host->clock = Py_NewRef(clock);
    return 0;
}

void ta_python_clock_detach(PyObject *clock, ta_python_host *host) {
    PyObject *hosts = ((ta_python_clock *)clock)->hosts;
    for (Py_ssize_t i = hosts ? PyList_Size(hosts) - 1 : -1; i >= 0; i--) {
        PyObject *referent = NULL;
        if (PyWeakref_GetRef(PyList_GetItem(hosts, i), &referent) < 0) PyErr_Clear();
        if (!referent || referent == (PyObject *)host) PySequence_DelItem(hosts, i);
        Py_XDECREF(referent);
    }
}

/* increment(ms): the clock moves on by `ms`, and each of its hosts runs
 * its turn at the new time (ta_host_advance_time, ta_host_run until idle).
 * The first error a host's turn raises is raised after every host ran. */
static PyObject *ta_python_clock_increment(PyObject *self, PyObject *argument) {
    ta_python_clock *clock = (ta_python_clock *)self;
    unsigned long long milliseconds = PyLong_Check(argument) ? PyLong_AsUnsignedLongLong(argument) : 0;
    if (!PyLong_Check(argument)) PyErr_SetString(PyExc_TypeError, "increment takes an int of milliseconds");
    if (PyErr_Occurred()) return NULL;
    PyObject *hosts = PySequence_List(clock->hosts); /* a turn may close or make hosts */
    if (!hosts) return NULL;
    clock->milliseconds += milliseconds;
    PyObject *raised = NULL;
    for (Py_ssize_t i = 0; i < PyList_Size(hosts); i++) {
        PyObject *referent = NULL;
        if (PyWeakref_GetRef(PyList_GetItem(hosts, i), &referent) <= 0) continue;
        ta_python_host *host = (ta_python_host *)referent;
        if (host->host && ta_python_host_usable(host)) ta_python_loop_turn(host);
        if (PyErr_Occurred() && !raised) raised = PyErr_GetRaisedException();
        PyErr_Clear();
        Py_DECREF(referent);
    }
    Py_DECREF(hosts);
    if (raised) {
        PyErr_SetRaisedException(raised);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef ta_python_clock_methods[] = {
    {"increment", ta_python_clock_increment, METH_O,
     "increment(ms)\n\nMoves the clock on by ms milliseconds and runs each of its hosts until idle at the new time."},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot ta_python_clock_slots[] = {
    {Py_tp_doc, "SimulatedClock()\n\nA clock for hosts made with it: their time moves only with increment(ms), and "
                "they arm no timer."},
    {Py_tp_new, ta_python_clock_new},
    {Py_tp_dealloc, ta_python_clock_dealloc},
    {Py_tp_traverse, ta_python_clock_traverse},
    {Py_tp_clear, ta_python_clock_clear},
    {Py_tp_methods, ta_python_clock_methods},
    {0, NULL},
};

static PyType_Spec ta_python_clock_spec = {
    .name = "tinyactors.SimulatedClock",
    .basicsize = sizeof(ta_python_clock),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = ta_python_clock_slots,
};

int ta_python_clock_init_type(PyObject *module, ta_python_state *state) {
    state->clock_type = PyType_FromModuleAndSpec(module, &ta_python_clock_spec, NULL);
    if (!state->clock_type) return -1;
    return PyModule_AddObjectRef(module, "SimulatedClock", state->clock_type);
}

/* ---- the time of a turn ----------------------------------------------------------------------- */

/* `amount` of `per_second` units in the host's ticks. */
typedef struct ta_python_duration {
    uint64_t amount, per_second;
} ta_python_duration;

static ta_time ta_python_ticks(const ta_python_host *host, const ta_python_duration *duration) {
    uint64_t rate = host->ticks_per_second;
    return duration->amount / duration->per_second * rate + duration->amount % duration->per_second * rate /
                                                                  duration->per_second;
}

/* A clock of the time module in nanoseconds: monotonic_ns or time_ns
 * (the limited API has no PyTime_Monotonic before 3.13's full API). */
static int ta_python_nanoseconds(const ta_python_host *host, const char *clock, uint64_t *out) {
    PyObject *value = PyObject_CallMethod(ta_python_host_state(host)->time, clock, NULL);
    *out = value ? PyLong_AsUnsignedLongLong(value) : 0;
    Py_XDECREF(value);
    return PyErr_Occurred() ? -1 : 0;
}

int ta_python_loop_start(ta_python_host *host) {
    return ta_python_nanoseconds(host, "monotonic_ns", &host->origin_ns);
}

/* Both clocks now (bindings.md, section 3, "Both clocks in one call"): a
 * SimulatedClock's time, which is the wall clock too; else the monotonic
 * clock since the host was made and the time of day. The monotonic time is
 * in the host's ticks (host.config). */
static int ta_python_loop_now(const ta_python_host *host, ta_host_time *out) {
    if (host->clock) {
        uint64_t milliseconds = ((ta_python_clock *)host->clock)->milliseconds;
        out->monotonic = ta_python_ticks(host, &(ta_python_duration){milliseconds, 1000});
        out->wall_ns = milliseconds * 1000000u;
        return 0;
    }
    uint64_t monotonic = 0, wall = 0;
    if (ta_python_nanoseconds(host, "monotonic_ns", &monotonic) < 0 || ta_python_nanoseconds(host, "time_ns", &wall) < 0)
        return -1;
    uint64_t elapsed = monotonic > host->origin_ns ? monotonic - host->origin_ns : 0;
    out->monotonic = ta_python_ticks(host, &(ta_python_duration){elapsed, 1000000000u});
    out->wall_ns = wall;
    return 0;
}

/* ---- the turn --------------------------------------------------------------------------------- */

/* The first error of the turn is kept; later ones are dropped. */
static void ta_python_loop_keep(ta_python_host *host) {
    if (!PyErr_Occurred()) return;
    if (!host->raised) host->raised = PyErr_GetRaisedException();
    else PyErr_Clear();
}

/* A run that cannot go on (bindings.md, section 3): TA_RUN_EXHAUSTED, the
 * allocator refused, is Python's MemoryError, and the next turn goes on
 * where it stopped; TA_RUN_FAULT is reported with its detail, a status
 * when the run was called during a run (host.c: TA_ERROR_STATE). */
static void ta_python_loop_stopped(ta_python_host *host, const ta_run *run) {
    if (run->status == TA_RUN_EXHAUSTED) { /* with the pool core names, TA_POOL_* */
        PyObject *arguments = Py_BuildValue("(sI)", "ta_host_run: TA_RUN_EXHAUSTED", run->detail);
        if (arguments) PyErr_SetObject(PyExc_MemoryError, arguments);
        Py_XDECREF(arguments);
    } else {
        int32_t detail = (int32_t)run->detail;
        ta_python_raise_status(ta_python_host_state(host), "ta_host_run: TA_RUN_FAULT",
                               detail < 0 ? detail : TA_ERROR_FATAL);
    }
    ta_python_loop_keep(host);
}

/* Runs until idle at `now`: every status of section 3. */
static ta_run ta_python_loop_runs(ta_python_host *host) {
    ta_run run;
    for (;;) {
        run = ta_host_run(host->host, TA_PYTHON_BUDGET);
        const ta_host_effect *effects = NULL;
        uint32_t count = ta_host_effects(host->host, &effects);
        ta_python_effects_dispatch(host, &(ta_python_batch){effects, count});
        ta_host_effects_ack(host->host); /* whatever a handler raised */
        if (ta_python_datamodel_interrupted(&host->python))
            return run; /* a chart's evaluation raised KeyboardInterrupt or SystemExit: the turn stops */
        switch (run.status) {
        case TA_RUN_IDLE: return run;
        case TA_RUN_EXHAUSTED:
        case TA_RUN_FAULT: ta_python_loop_stopped(host, &run); return run;
        case TA_RUN_YIELDED: /* no event loop to give a turn to: let a signal stop it */
            if (PyErr_CheckSignals() < 0) {
                ta_python_loop_keep(host);
                return run;
            }
            break;
        default: break; /* TA_RUN_EFFECTS_FULL, TA_RUN_EFFECTS: run again now */
        }
    }
}

int ta_python_loop_turn(ta_python_host *host) {
    if (host->looping) { /* an input from a handler: this turn runs again */
        host->pending = 1;
        return 0;
    }
    host->looping = 1;
    ta_run run;
    memset(&run, 0, sizeof run);
    run.next_deadline = UINT64_MAX;
    do {
        host->pending = 0;
        ta_host_time now;
        if (ta_python_loop_now(host, &now) < 0) {
            ta_python_loop_keep(host);
            break;
        }
        ta_host_advance_time(host->host, now);
        run = ta_python_loop_runs(host);
    } while (host->pending && (run.status == TA_RUN_IDLE || run.status == TA_RUN_YIELDED) &&
             !(ta_python_datamodel_interrupted(&host->python)));
    host->looping = 0;
    host->next_deadline = run.next_deadline;
    if (ta_python_datamodel_settle(&host->python) < 0) {
        /* a run's BaseException comes first; a handler's exception becomes
         * its __context__ when it has none */
        PyObject *kept = PyErr_GetRaisedException();
        PyObject *context = kept ? PyException_GetContext(kept) : NULL;
        if (host->raised && kept && !context) PyException_SetContext(kept, host->raised); /* steals it */
        else Py_XDECREF(host->raised);
        Py_XDECREF(context);
        host->raised = kept;
    }
    if (host->closing) ta_python_host_free(host);
    if (!host->raised) return 0;
    PyErr_SetRaisedException(host->raised);
    host->raised = NULL;
    return -1;
}

/* ---- the synchronous driver ------------------------------------------------------------------ */

/* Host.run(): turns on the real clock, sleeping until each next deadline,
 * until the host has none. With a SimulatedClock, one turn: its time
 * moves only with increment(ms). */
PyObject *ta_python_loop_run(ta_python_host *host, PyObject *unused) {
    (void)unused;
    if (!ta_python_host_usable(host) || ta_python_loop_turn(host) < 0) return NULL;
    if (host->clock) Py_RETURN_NONE;
    PyObject *time = ta_python_host_state(host)->time;
    while (host->host && host->next_deadline != UINT64_MAX) {
        ta_host_time now;
        if (ta_python_loop_now(host, &now) < 0) break;
        double seconds = host->next_deadline > now.monotonic
                             ? (double)(host->next_deadline - now.monotonic) / (double)host->ticks_per_second
                             : 0.0;
        PyObject *slept = PyObject_CallMethod(time, "sleep", "d", seconds);
        if (!slept) break;
        Py_DECREF(slept);
        if (ta_python_loop_turn(host) < 0) break;
    }
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}
