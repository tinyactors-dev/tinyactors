/* The order, run the same way in each language (tools/precompile): a host
 * with one precompiled datamodel, one order actor, its events, a
 * hibernation and a wake between them, and what the actor logs and ends
 * with, printed. Each language's example calls it with its own binding. */
#ifndef TA_PRECOMPILE_DEMO_H
#define TA_PRECOMPILE_DEMO_H

#include <tinyactors/datamodel.h>

/* the chart's path; 0 when the order ended as expected */
int ta_demo_order(const char *chart_path, ta_datamodel_binding binding);

#endif
