/* Hosted actor memory-scaling benchmark. */
#include "tinyactors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct pool { size_t live, peak; } pool_t;
typedef struct bench { pool_t pool; ta_arena_t *arena; ta_system_t *system;
    ta_definition_t *definition; ta_actor_ref_t *actors; } bench_t;

static ta_status_t acquire(void *user,const ta_layout_t *request,ta_chunk_t *out)
{
    pool_t *p=user;size_t a=request->alignment<sizeof(void *)?sizeof(void *):request->alignment;
    void *raw=malloc(request->size+a-1+sizeof(void *));uintptr_t q;
    if(raw==NULL)return TA_NO_MEMORY;
    q=((uintptr_t)raw+sizeof(void *)+a-1)&~(uintptr_t)(a-1);
    ((void **)q)[-1]=raw;*out=(ta_chunk_t){(void *)q,*request};p->live+=request->size;
    if(p->live>p->peak)p->peak=p->live;return TA_OK;
}
static void release(void *user,const ta_chunk_t *chunk)
{ pool_t *p=user;p->live-=chunk->layout.size;free(((void **)chunk->data)[-1]); }

static void run(bench_t *b,size_t units)
{ ta_run_result_t result;assert(ta_system_run(b->system,&(ta_run_options_t){1,{units,1}},&result)==TA_OK); }

static void setup(bench_t *b)
{
    ta_chunk_allocator_t allocator;ta_system_config_t config;ta_definition_result_t result;
    const ta_chart_t chart={.name="memory",.data_model="memory",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},
        .states=(ta_chart_state_t[]){{.id="idle"},{0}}};
    allocator=(ta_chunk_allocator_t){&b->pool,acquire,release};
    assert(ta_arena_create(&allocator,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&b->arena)==TA_OK);
    ta_system_config_init(&config);config.session_prefix=ta_text_from_cstr("memory");
    config.data_model=ta_text_from_cstr("memory");
    assert(ta_system_create(b->arena,&config,&b->system)==TA_OK);
    assert(ta_definition_build(b->system,&chart,&result)==TA_OK);b->definition=result.definition;
}

int main(int argc,char **argv)
{
    bench_t b={0};size_t count=argc>1?(size_t)strtoull(argv[1],NULL,10):100000;
    size_t i,baseline,used; if(count==0){fprintf(stderr,"usage: %s [actors]\n",argv[0]);return 2;}
    setup(&b);baseline=b.pool.live;b.actors=malloc(count*sizeof(*b.actors));assert(b.actors!=NULL);
    for(i=0;i<count;++i)assert(ta_actor_spawn(b.system,&(ta_spawn_options_t){.definition=b.definition},&b.actors[i])==TA_OK);
    run(&b,count*4u);used=b.pool.live-baseline;
    printf("actors=%zu fixed_bytes=%zu live_bytes=%zu bytes_per_actor=%.2f peak_arena_bytes=%zu\n",
        count,baseline,used,(double)used/(double)count,b.pool.peak);
    for(i=0;i<count;++i)assert(ta_actor_stop(b.system,b.actors[i])==TA_OK);
    run(&b,count*4u);
    for(i=0;i<count;++i)assert(ta_actor_release(b.system,b.actors[i])==TA_OK);
    free(b.actors);ta_definition_release(b.definition);assert(ta_system_destroy(b.system)==TA_OK);
    ta_arena_close(b.arena);assert(b.pool.live==0);puts("baseline_restored=yes");return 0;
}
