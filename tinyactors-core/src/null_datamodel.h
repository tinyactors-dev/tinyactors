#pragma once

#include "datamodel.h"

/*
 * The SCXML Null data model (SCXML B.1, ADR 0057), declared publicly as
 * ta_null_datamodel(). Its whole expression language is the `In(state)`
 * predicate in a condition; everything that needs data is an error. See
 * docs/internal/datamodel-null-profile.md.
 */
