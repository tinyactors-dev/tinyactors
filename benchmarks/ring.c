/* Hosted actor-to-actor message ring benchmark. */
#define _POSIX_C_SOURCE 199309L
#include "tinyactors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct pool { size_t live, peak; } pool_t;
typedef struct bench { pool_t pool; ta_arena_t *arena; ta_system_t *system;
    ta_definition_t *definition; ta_actor_ref_t *actors; size_t count, hops, seen; } bench_t;

static ta_status_t acquire(void *user, const ta_layout_t *request, ta_chunk_t *out)
{
    pool_t *p=user; size_t a=request->alignment<sizeof(void *)?sizeof(void *):request->alignment;
    void *raw=malloc(request->size+a-1+sizeof(void *)); uintptr_t q;
    if(raw==NULL)return TA_NO_MEMORY;
    q=((uintptr_t)raw+sizeof(void *)+a-1)&~(uintptr_t)(a-1);
    ((void **)q)[-1]=raw;*out=(ta_chunk_t){(void *)q,*request};p->live+=request->size;
    if(p->live>p->peak)p->peak=p->live;return TA_OK;
}
static void release(void *user,const ta_chunk_t *chunk)
{ pool_t *p=user;p->live-=chunk->layout.size;free(((void **)chunk->data)[-1]); }

static ta_status_t forward(ta_context_t *context, ta_action_kind_t kind, ta_code_t code)
{
    bench_t *b=ta_context_host_user(context); ta_context_view_t view; ta_send_t send={0};
    size_t i;
    (void)kind;(void)code;ta_context_inspect(context,&view);++b->seen;
    if(b->seen>b->hops)return TA_OK;
    send.event=ta_text_from_cstr("hop");send.target.kind=TA_TARGET_INSTANCE;
    for(i=0;i<b->count;++i)if(b->actors[i].id==view.actor.id)break;
    assert(i<b->count);send.target.instance=b->actors[(i+1)%b->count];
    return ta_context_send(context,&send,NULL);
}

static uint64_t now_ns(void)
{ struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
  return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }

int main(int argc,char **argv)
{
    bench_t b={0};ta_chunk_allocator_t allocator;ta_system_config_t config;
    ta_definition_result_t result;ta_run_result_t run;size_t i,actors=64,hops=1000000;
    uint64_t start,end;char *tail;
    if(argc>1)actors=(size_t)strtoull(argv[1],&tail,10);
    if(argc>2)hops=(size_t)strtoull(argv[2],&tail,10);
    if(actors==0||hops==0){fprintf(stderr,"usage: %s [actors] [hops]\n",argv[0]);return 2;}
    b.count=actors;b.hops=hops;b.actors=malloc(actors*sizeof(*b.actors));assert(b.actors!=NULL);
    allocator=(ta_chunk_allocator_t){&b.pool,acquire,release};
    assert(ta_arena_create(&allocator,&(ta_arena_options_t){256,65536,TA_UNLIMITED,65536},&b.arena)==TA_OK);
    ta_system_config_init(&config);config.data_model=ta_text_from_cstr("ring");
    config.session_prefix=ta_text_from_cstr("ring");config.host_user=&b;config.data.run=forward;
    assert(ta_system_create(b.arena,&config,&b.system)==TA_OK);
    { const ta_action_t action={.kind=TA_ACTION_CUSTOM,.as.custom={1}};
      const ta_chart_t chart={.name="ring",.data_model="ring",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={1,1}},
        .states=(ta_chart_state_t[]){{.id="ready",.transitions=(ta_chart_transition_t[]){{.event="hop",.content={&action,1}},{0}}},{0}}};
      assert(ta_definition_build(b.system,&chart,&result)==TA_OK);b.definition=result.definition; }
    for(i=0;i<actors;++i)assert(ta_actor_spawn(b.system,&(ta_spawn_options_t){.definition=b.definition},&b.actors[i])==TA_OK);
    assert(ta_system_run(b.system,&(ta_run_options_t){1,{actors*4u,1}},&run)==TA_OK);
    start=now_ns();
    assert(ta_post(b.system,&(ta_post_options_t){.target={TA_TARGET_INSTANCE,b.actors[0],{0}},.message={.name={"hop",3}}},NULL)==TA_OK);
    do { assert(ta_system_run(b.system,&(ta_run_options_t){1,{hops+actors*4u,1}},&run)==TA_OK); } while(b.seen<=hops);
    end=now_ns();assert(b.seen==hops+1);
    printf("actors=%zu hops=%zu elapsed_ns=%llu ns_per_hop=%.1f hops_per_sec=%.3f peak_arena_bytes=%zu baseline_restored=",
        actors,hops,(unsigned long long)(end-start),(double)(end-start)/(double)hops,
        (double)hops*1e9/(double)(end-start),b.pool.peak);
    for(i=0;i<actors;++i)assert(ta_actor_stop(b.system,b.actors[i])==TA_OK);
    assert(ta_system_run(b.system,&(ta_run_options_t){1,{actors*8u,1}},&run)==TA_OK);
    for(i=0;i<actors;++i)assert(ta_actor_release(b.system,b.actors[i])==TA_OK);
    ta_definition_release(b.definition);assert(ta_system_destroy(b.system)==TA_OK);ta_arena_close(b.arena);
    printf("yes\n");free(b.actors);return b.pool.live==0?0:1;
}
