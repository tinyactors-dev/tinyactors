#pragma once

#include "tinyactors.h"

/*
 * The SCXML Null data model (SCXML B.1), the minimum semantic profile:
 * conditions are the `In(state)` predicate and nothing else, and every
 * operation that needs data is an error. See
 * tinyactors-core/docs/internal/datamodel-null-profile.md.
 *
 * The adapter is stateless: it allocates nothing per definition and nothing
 * per actor (instance size 0), so one instance serves every system.
 */
ta_datamodel tat_null_datamodel(void);
