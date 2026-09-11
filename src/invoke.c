#include "internal.h"

typedef struct invoke_start {
    tai_actor_t *actor;
    const ta_invoke_desc_t *description;
    uint32_t owner;
    ta_context_t context;
    ta_text_t type, source;
    ta_body_t body;
    tai_span_t *span;
} invoke_start_t;

static uint64_t priority(uint64_t id)
{
    id ^= id >> 30; id *= 0xbf58476d1ce4e5b9ull;
    id ^= id >> 27; id *= 0x94d049bb133111ebull;
    return id ^ (id >> 31);
}

static tai_invocation_t *find_invocation(ta_system_t *system, ta_invocation_ref_t ref)
{
    tai_invocation_t *node = system->invocations;
    while (node != NULL && node->ref.id != ref.id)
        node = ref.id < node->ref.id ? node->left : node->right;
    return node;
}

static void rotate(ta_system_t *system, tai_invocation_t *node, uint8_t left)
{
    tai_invocation_t *child = left ? node->right : node->left;
    if (left) { node->right=child->left; if(child->left)child->left->parent=node; child->left=node; }
    else { node->left=child->right; if(child->right)child->right->parent=node; child->right=node; }
    child->parent=node->parent;
    if(node->parent==NULL)system->invocations=child;
    else if(node==node->parent->left)node->parent->left=child; else node->parent->right=child;
    node->parent=child;
}

static void register_invocation(tai_actor_t *actor, tai_invocation_t *entry)
{
    tai_invocation_t **link=&actor->system->invocations, *parent=NULL;
    while(*link!=NULL) { parent=*link; link=entry->ref.id<parent->ref.id?&parent->left:&parent->right; }
    *link=entry; entry->parent=parent;
    while(entry->parent!=NULL && priority(entry->ref.id)<priority(entry->parent->ref.id))
        rotate(actor->system,entry->parent,(uint8_t)(entry==entry->parent->right));
    entry->next=actor->extra->invocations;
    actor->extra->invocations=entry;
    ++actor->extra->invocation_count;
    actor->extra->request_bytes+=entry->capacity;
    ++actor->system->stats.invocations;
    tai_mutated(actor->system);
}

static void retire(tai_invocation_t *entry)
{
    ta_system_t *system=entry->system;
    tai_actor_t *actor=tai_actor_find(system,entry->actor);
    while(entry->left!=NULL || entry->right!=NULL)
        rotate(system,entry,(uint8_t)(entry->left==NULL ||
            (entry->right!=NULL && priority(entry->right->ref.id)<priority(entry->left->ref.id))));
    if(entry->parent==NULL)system->invocations=NULL;
    else if(entry==entry->parent->left)entry->parent->left=NULL; else entry->parent->right=NULL;
    if(actor!=NULL && actor->extra!=NULL) {
        tai_invocation_t **link=&actor->extra->invocations;
        while(*link!=NULL && *link!=entry)link=&(*link)->next;
        if(*link==entry)*link=entry->next;
        --actor->extra->invocation_count;
        actor->extra->request_bytes-=entry->capacity;
    }
    if(entry->cancel!=NULL) { tai_io_discard(entry->cancel); entry->cancel=NULL; }
    --system->stats.invocations;
    tai_mutated(system);
    ta_arena_close(entry->arena);
}

static ta_status_t identifier(tai_invocation_t *entry, ta_text_t requested)
{
    char buffer[27], digits[20];
    uint64_t id=entry->ref.id;
    size_t n=0, length=7;
    if(requested.data!=NULL)return tai_text_copy(entry->arena,requested,&entry->id);
    tai_copy(buffer,"invoke.",7);
    do { digits[n++]=(char)('0'+id%10); id/=10; } while(id!=0);
    while(n!=0)buffer[length++]=digits[--n];
    return tai_text_copy(entry->arena,(ta_text_t){buffer,length},&entry->id);
}

static ta_status_t external_start(tai_invocation_t *entry, invoke_start_t *start)
{
    tai_actor_t *actor=tai_actor_find(entry->system,entry->actor);
    ta_io_request_t request={0};
    ta_status_t status;
    request.actor=entry->actor;
    request.revision=actor->definition->revision;
    request.invocation=entry->ref;
    request.invokeid=entry->id;
    request.type=ta_text_from_cstr(entry->service->type);
    request.target=entry->source;
    request.accepted_ns=request.due_ns=entry->system->now_ns;
    request.kind=TA_IO_INVOKE_CANCEL;
    status=tai_io_open(actor,&request,&entry->cancel);
    if(status!=TA_OK)return status;
    entry->cancel->invocation=entry;
    request.kind=TA_IO_INVOKE_START;
    request.body=start->body;
    request.trace=start->span==NULL?start->context.view.trace:&start->span->value.context;
    status=tai_io_open(actor,&request,&entry->start);
    if(status!=TA_OK)return status;
    entry->start->invocation=entry;
    entry->start->span=start->span;start->span=NULL;
    tai_io_expose(entry->start);
    return TA_OK;
}

static ta_status_t create_invocation(invoke_start_t *start)
{
    tai_actor_t *actor=start->actor;
    ta_system_t *system=actor->system;
    const ta_service_desc_t *service=tai_service(system,start->type);
    tai_invocation_t *entry, *old;
    ta_arena_t *arena;
    ta_status_t status;
    ta_memory_stats_t memory;
    if(service==NULL)return TA_UNSUPPORTED;
    if(system->next_invocation==0 || system->stats.invocations>=system->config.limits.invocations ||
        actor->extra->invocation_count>=actor->limits->invocations)return TA_LIMIT;
    status=tai_scope(system,TA_LIFETIME_IO,&arena);
    if(status!=TA_OK)return status;
    status=tai_alloc(arena,sizeof(*entry),(void**)&entry);
    if(status!=TA_OK){ta_arena_close(arena);return status;}
    *entry=(tai_invocation_t){0};
    entry->system=system; entry->arena=arena; entry->actor.id=actor->id;
    entry->ref.id=system->next_invocation;
    system->next_invocation=entry->ref.id==UINT64_MAX?0:entry->ref.id+1;
    entry->owner=start->owner; entry->description=start->description; entry->service=service;
    entry->external=service!=&system->config.services[0];
    status=identifier(entry,start->description->id);
    if(status==TA_OK)status=tai_text_copy(arena,start->source,&entry->source);
    if(status!=TA_OK){ta_arena_close(arena);return status;}
    for(old=actor->extra->invocations;old!=NULL;old=old->next)
        if(!old->cancelled && tai_text_compare(old->id,entry->id)==0){ta_arena_close(arena);return TA_CONFLICT;}
    if(start->description->idlocation!=0) {
        ta_value_t value={0};
        status=system->config.data.from_text(system->config.host_user,entry->id,&value);
        if(status==TA_OK) {
            status=system->config.data.assign(&start->context,start->description->idlocation,value);
            if(value.handle!=0)system->config.values.release(system->config.host_user,value);
        }
        if(status!=TA_OK){ta_arena_close(arena);return status;}
    }
    ta_arena_stats(arena,&memory); entry->capacity=memory.reserved_bytes;
    if(actor->extra->request_bytes>actor->limits->io_request_bytes ||
        entry->capacity>actor->limits->io_request_bytes-actor->extra->request_bytes){ta_arena_close(arena);return TA_LIMIT;}
    register_invocation(actor,entry);
    if(entry->external)status=external_start(entry,start);
    else {
        ta_spawn_options_t options={0};
        tai_actor_t *child;
        options.definition=start->description->inline_definition;
        if(options.definition==NULL)options.definition_key=entry->source;
        options.parameters=start->body.parameters;
        options.parameter_count=start->body.parameter_count;
        options.trace=start->span==NULL?start->context.view.trace:&start->span->value.context;
        status=tai_spawn_actor(system,&options,&entry->child);
        if(status==TA_OK) {
            child=tai_actor_find(system,entry->child);
            status=tai_extra(child);
            if(status==TA_OK){child->extra->parent_invocation=entry;entry->started=1;}
            else {tai_release_child(child);entry->child.id=0;}
        }
    }
    if(status!=TA_OK){if(entry->start!=NULL)tai_io_discard(entry->start);retire(entry);}
    return status;
}

ta_status_t tai_invoke_pending(tai_actor_t *actor,uint8_t *worked)
{
    const ta_definition_desc_t *d=&actor->definition->description;
    uint32_t state,index;
    ta_status_t status;
    invoke_start_t start={0};
    *worked=0;
    for(state=0;state<d->state_count;++state) {
        index=actor->extra!=NULL && actor->extra->invoke_progress!=NULL?actor->extra->invoke_progress[state]:0;
        if(tai_active(actor,state) && index<d->states[state].invoke_count)break;
    }
    if(state==d->state_count)return TA_OK;
    status=tai_extra(actor);
    if(status!=TA_OK)return status;
    if(actor->extra->invoke_progress==NULL) {
        tai_array_layout_t layout={d->state_count,sizeof(uint32_t),TAI_ALIGNMENT};
        status=tai_array(actor->extra->arena,&layout,(void**)&actor->extra->invoke_progress);
        if(status!=TA_OK)return status;
        tai_zero(actor->extra->invoke_progress,(size_t)d->state_count*sizeof(uint32_t));
    }
    index=actor->extra->invoke_progress[state]++;
    *worked=1;
    start.actor=actor; start.owner=state; start.description=&d->states[state].invokes[index];
    tai_context(actor,TA_PHASE_INVOKE,&start.context);
    start.context.view.state_id=state;
    status=tai_text_eval(&start.context,&start.description->type,&start.type);
    if(status==TA_OK)status=tai_text_eval(&start.context,&start.description->source,&start.source);
    if(status==TA_OK)status=tai_body_eval(&start.context,&start.description->body,&start.body);
    if(status==TA_OK) {
        ta_span_start_t span={.operation=TA_SPAN_INVOKE,.kind=TA_SPAN_CLIENT,
            .actor={actor->id},.revision=actor->definition->revision,.destination=start.source,
            .processor=start.type,.now_ns=actor->system->now_ns,.parent=start.context.view.trace};
        tai_span_begin(actor->system,&span,&start.span);
        status=create_invocation(&start);
        tai_span_finish(start.span,status);
    }
    return status;
}

void tai_invoke_cancel_state(tai_actor_t *actor,uint32_t state)
{
    tai_invocation_t *entry,*next;
    if(actor->extra==NULL)return;
    if(actor->extra->invoke_progress!=NULL) {
        if(state==TA_INDEX_NONE)tai_zero(actor->extra->invoke_progress,(size_t)actor->definition->description.state_count*sizeof(uint32_t));
        else actor->extra->invoke_progress[state]=0;
    }
    for(entry=actor->extra->invocations;entry!=NULL;entry=next) {
        next=entry->next;
        if((entry->owner!=state && state!=TA_INDEX_NONE) || entry->cancelled)continue;
        entry->cancelled=1;
        tai_mutated(actor->system);
        if(!entry->external) {
            tai_actor_t *child=tai_actor_find(actor->system,entry->child);
            if(child!=NULL && child->status<TA_ACTOR_DONE) {
                child->stop=1;child->status=TA_ACTOR_STOPPING;child->stable=0;tai_ready(child);
            } else retire(entry);
        } else if(entry->start!=NULL) {
            if(!entry->start->taken){tai_io_discard(entry->start);entry->start=NULL;retire(entry);}
        } else if(entry->done)retire(entry);
        else if(entry->cancel!=NULL)tai_io_expose(entry->cancel);
    }
}

static ta_status_t return_event(tai_invocation_t *entry,const ta_message_t *message,uint8_t done)
{
    tai_actor_t *actor=tai_actor_find(entry->system,entry->actor);
    tai_delivery_t delivery={0};
    ta_status_t status;
    ta_arena_t *scratch=NULL;
    if(entry->cancelled || entry->done || !entry->started)return TA_STALE;
    if(actor==NULL || !tai_accepting(actor))return TA_CANCELLED;
    delivery.event.name=message->name;
    delivery.event.data=message->data;
    delivery.event.trace=message->trace;
    delivery.event.type=TA_EVENT_EXTERNAL;
    delivery.event.invokeid=entry->id;
    delivery.event.origin=entry->source;
    delivery.event.origintype=ta_text_from_cstr(entry->service->event_processor);
    delivery.invocation=entry->ref;
    if(!entry->external) {
        tai_actor_t *child=tai_actor_find(entry->system,entry->child);
        if(child!=NULL)delivery.event.origin=tai_session(child,1);
        delivery.event.origintype=ta_text_from_cstr("scxml");
    }
    if(done) {
        char *name;
        if(entry->id.size>(size_t)-1-12)return TA_LIMIT;
        status=ta_arena_open(entry->arena,&entry->system->config.arenas[TA_LIFETIME_MESSAGE],&scratch);
        if(status!=TA_OK)return status;
        status=tai_alloc(scratch,entry->id.size+12,(void**)&name);
        if(status!=TA_OK){ta_arena_close(scratch);return status;}
        tai_copy(name,"done.invoke.",12);tai_copy(name+12,entry->id.data,entry->id.size);
        delivery.event.name=(ta_text_t){name,entry->id.size+12};
    }
    status=tai_enqueue(actor,&delivery,NULL);
    if(scratch!=NULL)ta_arena_close(scratch);
    if(status==TA_OK && done){entry->done=1;tai_mutated(entry->system);}
    return status;
}

ta_status_t ta_invocation_emit(ta_system_t *system,ta_invocation_ref_t ref,const ta_message_t *message)
{
    tai_invocation_t *entry;
    if(system==NULL || message==NULL)return TA_INVALID;
    if(system->running)return TA_BUSY;
    entry=find_invocation(system,ref);
    if(entry==NULL || !entry->external)return TA_STALE;
    return return_event(entry,message,0);
}

static ta_status_t return_done(tai_invocation_t *entry,const ta_body_t *body,const ta_trace_context_t *trace)
{
    tai_actor_t *actor=tai_actor_find(entry->system,entry->actor);
    ta_message_t message={0};
    ta_context_t context;
    ta_status_t status;
    ta_arena_t *scratch;
    tai_actor_t candidate;
    tai_actor_extra_t extra;
    if(actor==NULL)return TA_CANCELLED;
    status=tai_actor_scope(actor,TA_LIFETIME_MESSAGE,&scratch);
    if(status!=TA_OK)return status;
    candidate=*actor;extra=*actor->extra;extra.macrostep=scratch;candidate.extra=&extra;
    tai_context(&candidate,TA_PHASE_INVOKE,&context);
    message.trace=trace;
    status=tai_body_payload(&context,body,&message.data);
    if(status==TA_OK)status=return_event(entry,&message,1);
    ta_arena_close(scratch);
    return status;
}

ta_status_t ta_invocation_done(ta_system_t *system,ta_invocation_ref_t ref,const ta_body_t *body)
{
    tai_invocation_t *entry;
    if(system==NULL || body==NULL)return TA_INVALID;
    if(system->running)return TA_BUSY;
    entry=find_invocation(system,ref);
    if(entry==NULL || !entry->external || entry->cancelled || entry->done || !entry->started)return TA_STALE;
    { ta_status_t status; system->running=1;status=return_done(entry,body,NULL);system->running=0;return status; }
}

static void report_failure(tai_invocation_t *entry)
{
    tai_actor_t *actor=tai_actor_find(entry->system,entry->actor);
    uint32_t owner=entry->owner;
    /* Error delivery can fail-stop and cancel every invocation. Unlink this
     * completed service first so cancellation cannot retire it recursively. */
    retire(entry);
    if(actor!=NULL && tai_accepting(actor)) {
        ta_context_t context;
        tai_context(actor,TA_PHASE_INVOKE,&context);
        context.view.state_id=owner;
        tai_execution_error(&context,TA_COMMUNICATION_ERROR,(ta_text_t){0});
    }
}

ta_status_t ta_invocation_fail(ta_system_t *system,ta_invocation_ref_t ref)
{
    tai_invocation_t *entry;
    if(system==NULL)return TA_INVALID;
    if(system->running)return TA_BUSY;
    entry=find_invocation(system,ref);
    if(entry==NULL || !entry->external || entry->cancelled || entry->done || !entry->started)return TA_STALE;
    report_failure(entry);return TA_OK;
}

ta_status_t ta_invocation_is_cancelled(ta_system_t *system,ta_invocation_ref_t ref,uint8_t *out)
{
    tai_invocation_t *entry;
    if(system==NULL || out==NULL)return TA_INVALID;
    entry=find_invocation(system,ref);
    if(entry==NULL)return TA_STALE;
    *out=entry->cancelled;return TA_OK;
}

void tai_invocation_completed(tai_io_t *request,ta_status_t result)
{
    tai_invocation_t *entry=request->invocation;
    if(entry==NULL)return;
    if(request->request.kind==TA_IO_INVOKE_START) {
        entry->start=NULL;
        if(result!=TA_OK) {if(!entry->cancelled)report_failure(entry);else retire(entry);}
        else {entry->started=1;if(entry->cancelled)tai_io_expose(entry->cancel);}
    } else if(request->request.kind==TA_IO_INVOKE_CANCEL) {entry->cancel=NULL;retire(entry);}
}

void tai_invoke_child_terminal(tai_actor_t *child,const ta_body_t *body)
{
    tai_invocation_t *entry=child->extra==NULL?NULL:child->extra->parent_invocation;
    ta_status_t status;
    if(entry==NULL)return;
    if(entry->cancelled || body==NULL) {
        entry->child.id=0;
        if(!entry->cancelled && !child->stop)report_failure(entry);else retire(entry);
    } else {
        status=return_done(entry,body,child->extra->trace);
        entry->child.id=0;
        if(status!=TA_OK)report_failure(entry);
    }
    /* Scheduler observes the non-NULL marker and releases the terminal child
     * after the complete interpreter unit, without dereferencing the record. */
}

static void autoforward(tai_actor_t *actor,tai_invocation_t *entry,const ta_event_t *event)
{
    ta_status_t status;
    tai_span_t *span=NULL;
    ta_span_start_t start={.operation=TA_SPAN_SEND,.kind=TA_SPAN_CLIENT,.actor={actor->id},
        .revision=actor->definition->revision,.message_id=event->message_id,.event_name=event->name,
        .destination=entry->id,.processor=ta_text_from_cstr(entry->service->event_processor),
        .now_ns=actor->system->now_ns,.parent=actor->extra->trace,
        .links=event->trace,.link_count=event->trace==NULL?0:1};
    tai_span_begin(actor->system,&start,&span);
    if(!entry->external) {
        tai_actor_t *child=tai_actor_find(actor->system,entry->child);
        tai_delivery_t delivery={0};
        if(child==NULL || !tai_accepting(child))status=TA_COMMUNICATION_ERROR;
        else {delivery.event=*event;status=tai_enqueue(child,&delivery,NULL);}
    } else {
        ta_io_request_t request={0};
        tai_io_t *io;
        request.kind=TA_IO_SEND;request.actor.id=actor->id;request.revision=actor->definition->revision;
        request.invocation=entry->ref;request.invokeid=entry->id;
        request.type=ta_text_from_cstr(entry->service->event_processor);request.target=entry->source;
        request.event=*event;request.event.data=(ta_payload_t){0};
        request.body.kind=TA_BODY_CONTENT;request.body.content=event->data;request.trace=event->trace;
        request.accepted_ns=request.due_ns=actor->system->now_ns;
        status=tai_io_open(actor,&request,&io);
        if(status==TA_OK){io->span=span;span=NULL;tai_io_expose(io);}
    }
    tai_span_finish(span,status);
    if(status!=TA_OK) {
        ta_context_t context;tai_context(actor,TA_PHASE_INVOKE,&context);
        tai_execution_error(&context,TA_COMMUNICATION_ERROR,event->sendid);
    }
}

void tai_invoke_event(tai_actor_t *actor)
{
    tai_invocation_t *entry,*next,*matched=NULL;
    ta_context_t context;
    ta_event_t scratch;
    const ta_event_t *event=tai_current_event(actor,&scratch);
    if(event==NULL || actor->extra==NULL)return;
    if(actor->current!=NULL && actor->current->invocation.id!=0) {
        matched=find_invocation(actor->system,actor->current->invocation);
        if(matched!=NULL && (matched->cancelled || matched->actor.id!=actor->id))matched=NULL;
    }
    if(matched!=NULL) {
        ta_status_t status=TA_OK;
        tai_context(actor,TA_PHASE_FINALIZE,&context);context.view.state_id=matched->owner;
        if(matched->description->finalize_kind==TA_FINALIZE_BLOCK)
            status=tai_execute(&context,&matched->description->finalize);
        else if(matched->description->finalize_kind==TA_FINALIZE_AUTOMATIC) {
            ta_finalize_call_t call={matched->description->body.parameters,matched->description->body.parameter_count,event->data};
            status=actor->system->config.data.finalize(&context,&call);
        }
        if(status!=TA_OK)tai_execution_error(&context,status,(ta_text_t){0});
        if(actor->status>=TA_ACTOR_DONE)return;
    }
    for(entry=actor->extra->invocations;entry!=NULL;entry=next) {
        next=entry->next;
        if(!entry->cancelled && !entry->done && entry->started && entry->description->autoforward)
            autoforward(actor,entry,event);
        if(actor->status>=TA_ACTOR_DONE)return;
    }
    if(matched!=NULL && matched->done)retire(matched);
}
