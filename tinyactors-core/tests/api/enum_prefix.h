// A constant outside the declared prefix would otherwise rename its peers.
// expect error: TA_RUNNING: does not start with the TA_SYMBOLS prefix TA_RUN_
typedef enum { TA_RUN_STABLE = 0, TA_RUNNING } ta_run TA_SYMBOLS("TA_RUN_");
