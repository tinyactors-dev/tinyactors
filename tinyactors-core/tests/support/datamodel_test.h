/* datamodel_test.h — a small ECMAScript-like datamodel for tests (hosted, not part of
 * the library). Registered under the name "ecmascript".
 *
 * Values: undefined, null, booleans, numbers, strings, arrays, objects.
 * Expressions: literals, [..], {k: v}, identifiers, a.b, a[i], calls In('s'),
 * unary ! - typeof, * / % + - < <= > >= == != === !==, && ||, and .length.
 * Scripts: statements separated by ';' : var x = e, loc = e, loc++, expr.
 * All memory lives in one arena freed by datamodel_test_free(). */
#ifndef TINYACTORS_TESTS_SUPPORT_DATAMODEL_TEST_H
#define TINYACTORS_TESTS_SUPPORT_DATAMODEL_TEST_H

#include <stddef.h>

#include <tinyactors/datamodel.h>

typedef struct datamodel_test datamodel_test;

datamodel_test *datamodel_test_new(void);
void datamodel_test_free(datamodel_test *datamodel);
ta_datamodel_binding datamodel_test_binding(datamodel_test *datamodel);

/* Stringify a variable of an instance ("<undeclared>" if missing). */
const char *datamodel_test_variable(datamodel_test *datamodel, uint64_t instance, const char *name);
/* Evaluate an expression in an instance and stringify ("<error>" on error). */
const char *datamodel_test_eval(datamodel_test *datamodel, uint64_t instance,
                                const char *expression);
/* Payload helpers for host-injected events. */
ta_payload datamodel_test_payload(datamodel_test *datamodel, const char *expression);
const char *datamodel_test_payload_string(datamodel_test *datamodel, ta_payload p);
/* Count of live instances (leak checks). */
int datamodel_test_live(datamodel_test *datamodel);

#endif
