#include "internal.h"

static uint64_t io_priority(uint64_t id)
{
    id ^= id >> 30; id *= UINT64_C(0xbf58476d1ce4e5b9); id ^= id >> 27;
    id *= UINT64_C(0x94d049bb133111eb); return id ^ (id >> 31);
}
static void io_left(tai_io_t **root, tai_io_t *e)
{
    tai_io_t *c=e->right; e->right=c->left; if(c->left)c->left->parent=e; c->parent=e->parent;
    if(!e->parent)*root=c; else if(e==e->parent->left)e->parent->left=c; else e->parent->right=c;
    c->left=e; e->parent=c;
}
static void io_right(tai_io_t **root, tai_io_t *e)
{
    tai_io_t *c=e->left; e->left=c->right; if(c->right)c->right->parent=e; c->parent=e->parent;
    if(!e->parent)*root=c; else if(e==e->parent->left)e->parent->left=c; else e->parent->right=c;
    c->right=e; e->parent=c;
}
static void io_insert(ta_system_t *s, tai_io_t *e)
{
    tai_io_t *p=NULL,*a=s->requests; while(a){p=a; a=e->request.request.id<a->request.request.id?a->left:a->right;}
    e->left=e->right=NULL; e->parent=p; if(!p)s->requests=e; else if(e->request.request.id<p->request.request.id)p->left=e; else p->right=e;
    while(e->parent && io_priority(e->request.request.id)<io_priority(e->parent->request.request.id))
        if(e==e->parent->left)io_right(&s->requests,e->parent); else io_left(&s->requests,e->parent);
}
static tai_io_t *io_find(ta_system_t *s, uint64_t id)
{
    tai_io_t *a=s->requests; while(a){if(a->request.request.id==id)return a; a=id<a->request.request.id?a->left:a->right;} return NULL;
}
static void io_remove(ta_system_t *s, tai_io_t *e)
{
    while(e->left||e->right) { if(!e->right||(e->left&&io_priority(e->left->request.request.id)<io_priority(e->right->request.request.id)))io_right(&s->requests,e); else io_left(&s->requests,e); }
    if(!e->parent)s->requests=NULL; else if(e->parent->left==e)e->parent->left=NULL; else e->parent->right=NULL; e->parent=NULL;
}
static void out_remove(tai_io_t *e)
{
    ta_system_t *s=e->system;
    if(!e->exposed)return;
    if(e->out_previous)e->out_previous->out_next=e->out_next; else s->outbox_head=e->out_next;
    if(e->out_next)e->out_next->out_previous=e->out_previous; else s->outbox_tail=e->out_previous;
    e->out_next=e->out_previous=NULL; e->exposed=0;
}
static void io_finish(tai_io_t *e, ta_status_t result)
{
    ta_system_t *s=e->system; tai_actor_t *a=tai_actor_find(s,e->request.actor);
    if(e->span) { tai_span_finish(e->span,result); e->span=NULL; }
    if(a!=NULL && a->extra!=NULL) { if(a->extra->request_count) --a->extra->request_count; if(a->extra->request_bytes>=e->capacity)a->extra->request_bytes-=e->capacity; else a->extra->request_bytes=0; }
    if(s->stats.io_requests) --s->stats.io_requests;
    io_remove(s,e); ta_arena_close(e->arena);
}

ta_status_t tai_io_open(tai_actor_t *actor, const ta_io_request_t *request, tai_io_t **out)
{
    ta_arena_t *arena; tai_io_t *e; tai_value_scope_t scope; ta_memory_stats_t ms; ta_status_t st; void *mem;
    if(out==NULL) return TA_INVALID;
    *out=NULL;
    if(actor==NULL||request==NULL||actor->system==NULL||request->request.id!=0) return TA_INVALID;
    if(request->kind==TA_IO_SEND && (actor->status==TA_ACTOR_DONE || actor->status==TA_ACTOR_STOPPED || actor->status==TA_ACTOR_FAILED)) return TA_CANCELLED;
    if(actor->system->next_request==0)return TA_LIMIT;
    if(actor->system->stats.io_requests>=actor->system->config.limits.io_requests)return TA_LIMIT;
    if(actor->limits==NULL) actor->limits=&actor->system->config.actor_defaults;
    st=tai_extra(actor); if(st!=TA_OK)return st;
    if(actor->extra->request_count>=actor->limits->io_requests||actor->extra->request_bytes>=actor->limits->io_request_bytes)return TA_LIMIT;
    st=tai_scope(actor->system,TA_LIFETIME_IO,&arena); if(st!=TA_OK)return st;
    st=tai_alloc(arena,sizeof(*e),&mem); if(st!=TA_OK){ta_arena_close(arena);return st;} e=(tai_io_t*)mem; *e=(tai_io_t){0}; e->system=actor->system;e->arena=arena;
    e->request=*request; e->request.request=(ta_request_ref_t){actor->system->next_request++}; e->request.actor=(ta_actor_ref_t){actor->id};
    scope=(tai_value_scope_t){actor->system,arena};
    st=tai_text_copy(arena,request->invokeid,&e->request.invokeid); if(st==TA_OK)st=tai_text_copy(arena,request->type,&e->request.type);
    if(st==TA_OK)st=tai_text_copy(arena,request->target,&e->request.target);
    if(st==TA_OK)st=tai_text_copy(arena,request->event.name,&e->request.event.name);
    if(st==TA_OK)st=tai_text_copy(arena,request->event.sendid,&e->request.event.sendid);
    if(st==TA_OK)st=tai_text_copy(arena,request->event.origin,&e->request.event.origin);
    if(st==TA_OK)st=tai_text_copy(arena,request->event.origintype,&e->request.event.origintype);
    if(st==TA_OK)st=tai_text_copy(arena,request->event.invokeid,&e->request.event.invokeid);
    if(st==TA_OK)st=tai_payload_snapshot(&scope,&request->event.data,&e->request.event.data);
    if(st==TA_OK)st=tai_body_snapshot(&scope,&request->body,&e->request.body);
    if(st==TA_OK) {
        st=tai_trace_copy(&scope,request->trace,&e->request.trace);
        if(st!=TA_OK) { e->request.trace=NULL; tai_saturate(&actor->system->stats.dropped_trace_records); st=TA_OK; }
    }
    e->request.event.trace=e->request.trace;
    if(st!=TA_OK){ta_arena_close(arena);return st;}
    ta_arena_stats(arena,&ms); e->capacity=ms.reserved_bytes; if(e->capacity>actor->limits->io_request_bytes-actor->extra->request_bytes){ta_arena_close(arena);return TA_LIMIT;}
    io_insert(actor->system,e); ++actor->system->stats.io_requests; ++actor->extra->request_count; actor->extra->request_bytes+=e->capacity; tai_mutated(actor->system); *out=e; return TA_OK;
}

void tai_io_expose(tai_io_t *e)
{ ta_system_t *s; if(e==NULL||e->exposed||e->taken)return; s=e->system; e->out_previous=s->outbox_tail;e->out_next=NULL; if(s->outbox_tail)s->outbox_tail->out_next=e;else s->outbox_head=e;s->outbox_tail=e;e->exposed=1; tai_mutated(s); }
void tai_io_discard(tai_io_t *e)
{ ta_system_t *s; if(e==NULL||e->taken)return; s=e->system; out_remove(e); io_finish(e,TA_CANCELLED); tai_mutated(s); }

ta_status_t ta_io_next(ta_system_t *s, const ta_io_request_t **out)
{ tai_io_t *e; if(s==NULL||out==NULL)return TA_INVALID; *out=NULL; if(s->running)return TA_BUSY; e=s->outbox_head; if(e==NULL)return TA_EMPTY; out_remove(e); e->taken=1; tai_mutated(s); *out=&e->request; return TA_OK; }

ta_status_t ta_io_complete(ta_system_t *s, ta_request_ref_t ref, ta_status_t result)
{
    tai_io_t *e; tai_actor_t *a; ta_context_t context;
    if(s==NULL||ref.id==0)return TA_INVALID;
    if(s->running)return TA_BUSY;
    if(result!=TA_OK&&result!=TA_COMMUNICATION_ERROR&&result!=TA_CANCELLED)return TA_INVALID;
    e=io_find(s,ref.id); if(e==NULL||!e->taken)return TA_STALE;
    a=tai_actor_find(s,e->request.actor);
    if(e->request.kind==TA_IO_SEND&&result!=TA_OK&&a!=NULL&&tai_accepting(a)){tai_context(a,TA_PHASE_INVOKE,&context);tai_execution_error(&context,TA_COMMUNICATION_ERROR,e->request.event.sendid);}
    if(e->request.kind!=TA_IO_SEND) tai_invocation_completed(e,result);
    io_finish(e,result); tai_mutated(s); return TA_OK;
}
