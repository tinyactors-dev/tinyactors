/* cli/decimal.h — a decimal number's text as the nearest double, ties to
 * even: Go's strconv.ParseFloat(s, 64), ported (the exact fast path, then
 * Eisel–Lemire, then the simple decimal conversion). Freestanding, no
 * allocation: the decimal's 800 digits are on the stack. The command's own:
 * in no released library. */
#ifndef TINYACTORS_CLI_DECIMAL_H
#define TINYACTORS_CLI_DECIMAL_H

#include <tinyactors/types.h>

typedef struct ta_decimal_reading {
    ta_string_view text; /* JSON's number grammar (RFC 8259, section 6), the whole text */
    double value;        /* set on TA_OK */
} ta_decimal_reading;

/* TA_OK; TA_ERROR_INVALID: not a number, or not finite as a double (its
 * magnitude rounds past the largest double). A value below the smallest
 * subnormal is ±0, as Go reads it. */
ta_status ta_decimal_read(ta_decimal_reading *reading);

#endif /* TINYACTORS_CLI_DECIMAL_H */
