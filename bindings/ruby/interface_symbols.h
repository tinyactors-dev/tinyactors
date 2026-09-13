/* One-to-one low-level coverage manifest. The checker extracts the
 * authoritative public list from tinyactors.h with clang. */
#define TINYACTORS_RUBY_C_SYMBOLS(X) \
    X(ta_arena_create) X(ta_arena_open) X(ta_arena_alloc) \
    X(ta_arena_defer) X(ta_arena_close) X(ta_arena_stats) \
    X(ta_arena_set_limit) X(ta_definition_prepare) X(ta_definition_build) \
    X(ta_definition_retain) X(ta_definition_release) X(ta_definition_revision) \
    X(ta_definition_description) X(ta_definition_footprint) \
    X(ta_definition_publish) X(ta_definition_unpublish) X(ta_definition_lookup) \
    X(ta_context_inspect) X(ta_context_in) X(ta_context_host_user) \
    X(ta_context_bytes) X(ta_context_arena) X(ta_system_config_init) \
    X(ta_system_create) X(ta_system_set_limits) X(ta_actor_spawn) \
    X(ta_actor_inspect) X(ta_actor_set_limits) X(ta_system_inspect) \
    X(ta_actor_stop) X(ta_actor_release) X(ta_name_bind) X(ta_name_unbind) \
    X(ta_name_lookup) X(ta_session_lookup) X(ta_target_parse) X(ta_post) \
    X(ta_context_raise) X(ta_context_send) X(ta_context_cancel) \
    X(ta_system_run) X(ta_system_next_deadline) X(ta_io_next) \
    X(ta_io_complete) X(ta_io_receive) X(ta_io_receive_error) \
    X(ta_invocation_emit) X(ta_invocation_done) X(ta_invocation_fail) \
    X(ta_invocation_is_cancelled) X(ta_actor_upgrade) X(ta_system_stats) \
    X(ta_system_shutdown) X(ta_system_destroy)
