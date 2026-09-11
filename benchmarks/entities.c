/* Hosted scale probe. Host malloc bookkeeping is not part of core byte totals. */
#include "tinyactors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ENTITY_COUNT = 200000 };
typedef struct entity { float x,y,vx,vy; uint32_t sprite,entity; } entity_t;
typedef struct pool { size_t bytes,peak,acquires; } pool_t;
typedef struct frame { size_t count; float x,y; } frame_t;
typedef struct benchmark {
    pool_t pool;
    ta_arena_t *root;
    ta_system_t *system;
    ta_definition_t *definition;
    ta_actor_ref_t *actors;
} benchmark_t;

static ta_status_t acquire(void *user, const ta_layout_t *request, ta_chunk_t *out)
{
    pool_t *pool=user;
    size_t alignment=request->alignment<sizeof(void *)?sizeof(void *):request->alignment;
    void *raw=malloc(request->size+alignment-1+sizeof(void *));
    uintptr_t pointer;
    if(raw==NULL)return TA_NO_MEMORY;
    pointer=((uintptr_t)raw+sizeof(void *)+alignment-1)&~(uintptr_t)(alignment-1);
    ((void **)pointer)[-1]=raw;*out=(ta_chunk_t){(void *)pointer,*request};
    pool->bytes+=request->size;++pool->acquires;if(pool->bytes>pool->peak)pool->peak=pool->bytes;
    return TA_OK;
}

static void release(void *user, const ta_chunk_t *chunk)
{
    pool_t *pool=user;pool->bytes-=chunk->layout.size;free(((void **)chunk->data)[-1]);
}

static ta_status_t tick(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    ta_mut_bytes_t bytes;
    entity_t *entity;
    (void)kind;(void)code;
    assert(ta_context_bytes(context,&bytes)==TA_OK && bytes.size==sizeof(*entity));
    entity=bytes.data;entity->x+=entity->vx;entity->y+=entity->vy;
    return TA_OK;
}

static ta_visit_t inspect(void *user, const ta_inspect_record_t *record)
{
    frame_t *frame=user;
    entity_t entity;
    assert(record->kind==TA_INSPECT_ACTOR && record->as.info.state.storage==TA_DATA_BYTES);
    memcpy(&entity,record->as.info.state.as.bytes.data,sizeof(entity));
    assert(entity.x==1 && entity.y==2 && entity.sprite==17);
    ++frame->count;frame->x+=entity.x;frame->y+=entity.y;return TA_VISIT_CONTINUE;
}

static void run(benchmark_t *b)
{
    ta_run_result_t result;
    assert(ta_system_run(b->system,&(ta_run_options_t){1,{ENTITY_COUNT*4u,1}},&result)==TA_OK);
    assert(result.runnable==0);
}

static void open_benchmark(benchmark_t *b)
{
    ta_system_config_t config;
    ta_definition_result_t result;
    const entity_t initial={0,0,1,2,17,0};
    const ta_action_t action={.kind=TA_ACTION_CUSTOM,.as.custom={1}};
    const ta_chart_t chart={.name="entity",.data_model="c-entity",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={sizeof(entity_t),sizeof(float)},
            .initial_bytes={&initial,sizeof(initial)}},
        .states=(ta_chart_state_t[]){{.id="alive",.transitions=(ta_chart_transition_t[]){
            {.event="tick",.content={&action,1}},{0}}},{0}}};
    *b=(benchmark_t){0};b->actors=malloc(ENTITY_COUNT*sizeof(*b->actors));assert(b->actors!=NULL);
    assert(ta_arena_create(&(ta_chunk_allocator_t){&b->pool,acquire,release},
        &(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&b->root)==TA_OK);
    ta_system_config_init(&config);config.data_model=ta_text_from_cstr("c-entity");
    config.session_prefix=ta_text_from_cstr("bench");config.data.run=tick;
    assert(ta_system_create(b->root,&config,&b->system)==TA_OK);
    assert(ta_definition_build(b->system,&chart,&result)==TA_OK);b->definition=result.definition;
}

static void round_trip(benchmark_t *b, unsigned round)
{
    size_t i,baseline=b->pool.bytes,idle,queued,allocations;
    clock_t start=clock(),admitted,initialized,posted,processed,inspected,retired;
    frame_t frame={0};
    ta_inspect_cursor_t cursor={{0}};
    ta_inspect_options_t options={.stable_only=1,.details=TA_INSPECT_DATA,.work_units=1024,.sink={&frame,inspect}};
    ta_status_t status;
    for(i=0;i<ENTITY_COUNT;++i)assert(ta_actor_spawn(b->system,&(ta_spawn_options_t){.definition=b->definition},&b->actors[i])==TA_OK);
    admitted=clock();run(b);initialized=clock();idle=b->pool.bytes-baseline;
    for(i=0;i<ENTITY_COUNT;++i)assert(ta_post(b->system,&(ta_post_options_t){
        .target={.kind=TA_TARGET_INSTANCE,.instance=b->actors[i]},.message={.name={"tick",4}}},NULL)==TA_OK);
    posted=clock();queued=b->pool.bytes-baseline;run(b);processed=clock();allocations=b->pool.acquires;
    do {status=ta_system_inspect(b->system,&options,&cursor);}while(status==TA_AGAIN);
    assert(status==TA_OK && frame.count==ENTITY_COUNT && allocations==b->pool.acquires);inspected=clock();
    for(i=0;i<ENTITY_COUNT;++i)assert(ta_actor_stop(b->system,b->actors[i])==TA_OK);
    run(b);
    for(i=0;i<ENTITY_COUNT;++i)assert(ta_actor_release(b->system,b->actors[i])==TA_OK);
    retired=clock();assert(b->pool.bytes==baseline);
    printf("round=%u actors=%u state_bytes=%zu idle_bytes=%zu bytes_per_actor=%.1f queued_bytes=%zu "
        "spawn_cpu_ms=%.1f initialize_cpu_ms=%.1f post_cpu_ms=%.1f process_cpu_ms=%.1f inspect_cpu_ms=%.1f retire_cpu_ms=%.1f baseline_restored=yes\n",
        round,ENTITY_COUNT,sizeof(entity_t),idle,(double)idle/ENTITY_COUNT,queued,
        1000.0*(admitted-start)/CLOCKS_PER_SEC,1000.0*(initialized-admitted)/CLOCKS_PER_SEC,
        1000.0*(posted-initialized)/CLOCKS_PER_SEC,1000.0*(processed-posted)/CLOCKS_PER_SEC,
        1000.0*(inspected-processed)/CLOCKS_PER_SEC,1000.0*(retired-inspected)/CLOCKS_PER_SEC);
}

int main(void)
{
    benchmark_t benchmark;
    open_benchmark(&benchmark);round_trip(&benchmark,1);round_trip(&benchmark,2);
    ta_definition_release(benchmark.definition);assert(ta_system_destroy(benchmark.system)==TA_OK);
    ta_arena_close(benchmark.root);assert(benchmark.pool.bytes==0);free(benchmark.actors);return 0;
}
