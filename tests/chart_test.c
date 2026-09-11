#include "host.h"
#include <assert.h>
#include <stdio.h>

static ta_status_t root_make(ta_test_allocator_t *h, ta_arena_t **out)
{
    ta_chunk_allocator_t a=ta_test_allocator_callbacks(h); ta_arena_options_t o={512,65536,(size_t)-1,65536};
    return ta_arena_create(&a,&o,out);
}

static ta_system_t *system_make(ta_test_allocator_t *h, ta_arena_t **root)
{
    ta_system_config_t c; ta_system_t *s=NULL;
    assert(root_make(h,root)==TA_OK); ta_system_config_init(&c); c.session_prefix=ta_text_from_cstr("chart"); c.data_model=ta_text_from_cstr("bytes");
    assert(ta_system_create(*root,&c,&s)==TA_OK); return s;
}

static void valid_chart(ta_system_t *s)
{
    ta_definition_result_t r; ta_chart_state_t leaf={.id="leaf"};
    ta_chart_transition_t tr[]={{.event="go stop",.target="stop"},{0}};
    ta_chart_state_t stop={.id="stop"}; ta_chart_state_t branch={.id="branch",.states=(const ta_chart_state_t[]){leaf,{0}},.transitions=tr};
    ta_chart_t c={.name="nested",.data_model="bytes",.data_layout={.storage=TA_DATA_BYTES,.bytes_layout={0,1}},.states=(const ta_chart_state_t[]){branch,stop,{0}},.initial={.target="branch"}};
    assert(ta_definition_build(s,&c,&r)==TA_OK); assert(r.definition!=NULL); { const ta_definition_desc_t *d=ta_definition_description(r.definition); assert(d->state_count==4u); assert(d->transition_count==1u); } ta_definition_release(r.definition);
}

static void lists_and_limits(ta_system_t *s)
{
    ta_definition_result_t r;
    ta_action_t raise = {.kind=TA_ACTION_RAISE,.as.raise={ {"notice",6} }};
    ta_chart_transition_t transitions[] = {
        {.event="  foo.* bar  ",.target="  child \t"},
        {.content={&raise,1}}, {0}
    };
    ta_chart_state_t children[] = {{.id="child"},{.id="sibling"},{0}};
    ta_chart_state_t states[] = {{.id="parent",.states=children,.transitions=transitions},{.id="other"},{0}};
    ta_chart_t chart = {.name="lists",.data_model="bytes",
        .data_layout={.storage=TA_DATA_BYTES,.bytes_layout={0,1}},.states=states};
    const ta_definition_desc_t *d;
    { ta_status_t status=ta_definition_build(s,&chart,&r);
      if(status!=TA_OK) fprintf(stderr,"chart status=%d state=%u transition=%u: %.*s\n",status,r.error.state,r.error.transition,(int)r.error.detail.size,r.error.detail.data);
      assert(status==TA_OK); }
    d=ta_definition_description(r.definition);
    assert(d->state_count==5 && d->states[1].kind==TA_STATE_COMPOUND);
    assert(d->states[2].parent==1 && d->states[3].parent==1 && d->states[4].parent==0);
    assert(d->transition_count==2 && d->transitions[0].event_count==2);
    assert(d->transitions[0].targets.count==1 && d->transitions[0].targets.states[0]==2);
    assert(d->transitions[1].event_count==0 && d->transitions[1].content.count==1);
    ta_definition_release(r.definition);
    transitions[0].event="";
    assert(ta_definition_build(s,&chart,&r)==TA_INVALID);
    transitions[0].event=" \t\n";
    assert(ta_definition_build(s,&chart,&r)==TA_INVALID);
    transitions[0].event="go"; transitions[0].target="missing";
    assert(ta_definition_build(s,&chart,&r)==TA_NOT_FOUND);
    transitions[0].target="child";
    children[2].onentry.count=1;
    assert(ta_definition_build(s,&chart,&r)==TA_INVALID);
    children[2]=(ta_chart_state_t){0};
    children[0].states=states;
    assert(ta_definition_build(s,&chart,&r)==TA_CONFLICT);
}

static void invalid_cases(ta_system_t *s)
{
    ta_definition_result_t r; ta_chart_state_t a={.id="a"};
    ta_chart_t c={.name="bad",.data_model="bytes",.states=(const ta_chart_state_t[]){a,a,{0}}};
    assert(ta_definition_build(s,&c,&r)==TA_CONFLICT);
    c.states=(const ta_chart_state_t[]){(ta_chart_state_t){.id="$root"},{0}}; assert(ta_definition_build(s,&c,&r)==TA_CONFLICT);
    c.states=(const ta_chart_state_t[]){(ta_chart_state_t){.kind=TA_CHART_FINAL},{0}}; assert(ta_definition_build(s,&c,&r)==TA_INVALID);
    { ta_chart_state_t shared={.id="shared"}; ta_chart_state_t p={.id="p",.states=(const ta_chart_state_t[]){shared,{0}}}; ta_chart_state_t q={.id="q",.states=(const ta_chart_state_t[]){shared,{0}}}; c.states=(const ta_chart_state_t[]){p,q,{0}}; assert(ta_definition_build(s,&c,&r)==TA_CONFLICT); }
}

int main(void)
{
    ta_test_allocator_t h; ta_arena_t *root; ta_system_t *s; ta_test_allocator_init(&h,0,0); s=system_make(&h,&root); valid_chart(s); invalid_cases(s); lists_and_limits(s); assert(ta_system_destroy(s)==TA_OK); ta_arena_close(root); assert(ta_test_allocator_releases_ok(&h)); ta_test_allocator_reset(&h); return 0;
}
