#include "internal.h"
#include "host.h"
#include <assert.h>

static ta_visit_t count_visit(void *user, const ta_inspect_record_t *record)
{
    size_t *count = (size_t *)user;
    (void)record;
    ++*count;
    return TA_VISIT_CONTINUE;
}

static void empty_scan(void)
{
    ta_system_t system = {0};
    ta_inspect_cursor_t cursor = {{0}};
    ta_inspect_options_t options = {0};
    size_t count = 0;
    options.work_units = 1;
    options.sink.user = &count;
    options.sink.visit = count_visit;
    assert(ta_system_inspect(&system, &options, &cursor) == TA_OK);
    assert(count == 0);
    assert(ta_system_inspect(&system, &options, &cursor) == TA_OK);
    options.work_units = 0;
    assert(ta_system_inspect(&system, &options, &cursor) == TA_INVALID);
}

typedef struct scan_log {
    ta_inspect_kind_t kinds[32];
    ta_actor_ref_t actors[32];
    size_t count;
    ta_visit_t next;
} scan_log_t;

static ta_visit_t visit(void *user, const ta_inspect_record_t *record)
{
    scan_log_t *log = user;
    assert(log->count < 32);
    log->kinds[log->count] = record->kind;
    log->actors[log->count++] = record->actor;
    if (record->kind == TA_INSPECT_HISTORY_STATE) {
        assert(!record->as.history.recorded);
        assert(record->as.history.target == TA_INDEX_NONE && record->as.history.ordinal == 0);
    }
    return log->next;
}

static void resumed_scan(void)
{
    ta_test_allocator_t host;
    ta_chunk_allocator_t callbacks;
    ta_arena_t *arena;
    ta_system_t *system;
    ta_system_config_t config;
    ta_definition_result_t definition;
    ta_actor_ref_t actors[3];
    ta_inspect_cursor_t cursor = {{0}};
    scan_log_t log = {0};
    ta_inspect_options_t options = {.details=TA_INSPECT_ACTIVE|TA_INSPECT_HISTORY,
        .work_units=1,.sink={&log,visit}};
    ta_chart_t chart = {.name="scan",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},
        .states=(ta_chart_state_t[]){{.id="box",.states=(ta_chart_state_t[]){
            {.id="a"},{.id="h",.kind=TA_CHART_HISTORY_DEEP,.initial={.target="a"}},{0}}},{0}}};
    ta_status_t status;
    ta_run_result_t run;
    size_t i, calls = 0, baseline;
    ta_test_allocator_init(&host,0,0); callbacks = ta_test_allocator_callbacks(&host);
    assert(ta_arena_create(&callbacks,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&arena)==TA_OK);
    ta_system_config_init(&config); config.data_model=ta_text_from_cstr("bytes");config.session_prefix=ta_text_from_cstr("scan");
    assert(ta_system_create(arena,&config,&system)==TA_OK);
    assert(ta_definition_build(system,&chart,&definition)==TA_OK);
    for(i=0;i<3;++i)assert(ta_actor_spawn(system,&(ta_spawn_options_t){.definition=definition.definition},&actors[i])==TA_OK);
    assert(ta_system_run(system,&(ta_run_options_t){1,{100,1}},&run)==TA_OK);
    baseline=host.live_bytes;
    log.next=TA_VISIT_PAUSE;
    do { status=ta_system_inspect(system,&options,&cursor); assert(++calls<30); } while(status==TA_AGAIN);
    assert(status==TA_OK && log.count==15 && host.live_bytes==baseline);
    for(i=0;i<15;++i) {
        assert(log.kinds[i]==(i%5==0?TA_INSPECT_ACTOR:(i%5==4?TA_INSPECT_HISTORY_STATE:TA_INSPECT_ACTIVE_STATE)));
        assert(log.actors[i].id==log.actors[(i/5)*5].id);
    }
    assert(log.actors[0].id!=log.actors[5].id && log.actors[5].id!=log.actors[10].id);
    log=(scan_log_t){0};options.details=0;options.revision=UINT64_MAX;calls=0;
    do {status=ta_system_inspect(system,&options,&cursor);++calls;}while(status==TA_AGAIN);
    assert(status==TA_OK && calls==3 && log.count==0); /* Each omitted actor spends a unit. */
    options.revision=0;options.details=TA_INSPECT_ACTIVE;log.next=TA_VISIT_PAUSE;
    assert(ta_system_inspect(system,&options,&cursor)==TA_AGAIN && log.count==1);
    assert(ta_post(system,&(ta_post_options_t){.target={.kind=TA_TARGET_INSTANCE,.instance=actors[0]},
        .message={.name={"tick",4}}},NULL)==TA_OK);
    assert(ta_system_inspect(system,&options,&cursor)==TA_STALE && log.count==1);
    cursor=(ta_inspect_cursor_t){{0}};log=(scan_log_t){.next=TA_VISIT_STOP};
    assert(ta_system_inspect(system,&options,&cursor)==TA_CANCELLED && cursor.opaque[0]==0);
    for(i=0;i<3;++i)assert(ta_actor_stop(system,actors[i])==TA_OK);
    assert(ta_system_run(system,&(ta_run_options_t){1,{100,1}},&run)==TA_OK);
    for(i=0;i<3;++i)assert(ta_actor_release(system,actors[i])==TA_OK);
    ta_definition_release(definition.definition);assert(ta_system_destroy(system)==TA_OK);
    ta_arena_close(arena);assert(ta_test_allocator_releases_ok(&host));ta_test_allocator_reset(&host);
}

int main(void)
{
    empty_scan();
    resumed_scan();
    return 0;
}
