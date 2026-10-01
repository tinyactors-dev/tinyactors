#ifndef TAT_HOSTED_H
#define TAT_HOSTED_H

#include "jq.h"

#include "binding.h"

/*
 * Hosted mode (kernel ADR 0001 section 5): systems whose actors are queued,
 * scheduled and routed by the host kernel (tinyactors-host), driven from
 * scenarios. Turns run only when a scenario asks; the clock is the
 * scenario's. Scenarios about core's semantics keep driving core directly.
 */
jv tat_hosted_create_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_close_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_spawn_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_post_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_turn_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_mailboxes_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_advance_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_record_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_records_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_evict_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_restore_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_set_loader_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_load_method(tat_bind *bind, jv params, int *error_code);
jv tat_hosted_stats_method(tat_bind *bind, jv params, int *error_code);

#endif
