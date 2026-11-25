/* A host for one chart, its loop and its payloads: what main.c uses. */
#ifndef HOST_H
#define HOST_H

#include <tinyactors.h>
#include <tinyactors_datamodel_ecmascript.h>
#include <tinyactors_runtime.h>

typedef struct host {
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodel;
    void *application; /* the program's own state, for its handler */
} host;

typedef struct host_options {
    const char *chart;        /* the chart's file */
    const char *system;       /* the system id */
    const char *io_processor; /* the one I/O processor type the chart sends to */
} host_options;

/* An event: its data an ECMAScript expression, or NULL for none. */
typedef struct host_event {
    uint64_t session;
    const char *name;
    const char *data;
} host_event;

/* Called for each effect of a batch, before the batch is acknowledged. */
typedef void host_handler(host *h, uint32_t effect);

ta_status host_open(host *h, const host_options *options);
uint64_t host_spawn(host *h, const char *chart);
ta_status host_send(host *h, host_event event);
/* Runs until nothing is left to do, timers included, handing each effect to `handle`. */
void host_run(host *h, host_handler *handle);
/* Answers the send `effect` with the event `name`, as its I/O processor. */
ta_status host_answer(host *h, uint32_t effect, const char *name);
/* Fails the send `effect`: its sender gets error.communication. */
ta_status host_fail(host *h, uint32_t effect);
/* Prints `label` and the effect's payload as JSON. */
void host_print(host *h, const char *label, uint32_t effect);
void host_close(host *h);

#endif
