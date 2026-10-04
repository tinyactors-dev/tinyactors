/*
** vmactor.c -- the parser actor: the bytecode VM hosted inside an ax actor (W60).
**
** This is the third route through pocketparse_finish, and the first that runs the
** parse genuinely on ax rather than using ax only as an arena allocator. The
** SAME bytecode VM (pp_execute_vm in vm.c, lowered by vmcompile.c) runs, but it
** runs inside one ax transition action: the engine's ax_sys spawns a parser
** actor, the host's input arrives as coarse input.chunk / input.end events, and
** the dispatch loop is plain C inside the action -- ax never sees a token or an
** opcode, only the two chunk/end events per parse. This is topology 2 ("VM
** managed by an actor") of docs/ax-topologies.md made real: ax owns the actor's
** lifetime and routes the coarse events; the tight loop stays in the action.
**
** The tree the actor builds is byte-identical to the in-process VM's, and so to
** the reference recursive descent's, because the action calls pp_execute_vm
** verbatim -- the actor is a HOST for the existing loop, not a reimplementation.
** ci/pocketparse-vm-diff.sh asserts the byte-identity across json + JS + TS.
**
** Memory: the parser actor's data block holds only a back-pointer to the
** pocketparse_parser and the bookkeeping the host reads after the run. The CST,
** the VM stacks, and the input buffer live where they always have -- in the
** parser's arena (carved from the engine grant) -- so the actor adds no copy.
** The actor itself, its mailbox, and its ax_heap live in the system slice
** pocketparse_open reserved for ax_open.
**
** Event flow:
**   pocketparse_parser_new  -> ax_spawn + post "actor.init" (parser pointer)
**   pocketparse_push        -> buffer the bytes straight into the parser arena
**   pocketparse_finish      -> post "input.end";  action runs the VM, reaches
**                              the AX_FINAL "done" state, writes the result back
**   each post is followed by ax_step pumped to quiescence (AX_IDLE).
**
** A push does NOT marshal its bytes through the ax mailbox. An earlier design
** posted each chunk as an "input.chunk" event whose payload ax_post copied into
** the system slice -- a fixed, small pool that pocketparse_open carves off the
** grant. On a reused engine that copy fragmented the slice (the pool's first-fit
** free list lets a tiny allocation claim a freed file-sized chunk), so after a
** couple of dozen parses of a large file a chunk-sized allocation failed,
** ax_post returned AX_NOMEM, and the push reported POCKETPARSE_INVALID. Worse,
** finish then still ran and produced a degenerate one-node tree with no fault.
** Buffering writes into the parser's OWN arena (carved from the growable engine
** pool, reset by pocketparse_parser_free), which is exactly where the input has
** always lived, so the actor genuinely adds no copy -- the contract this file
** states. The parse still runs inside the actor at input.end.
**
** Freestanding: no libc; all memory is arena/system memory.
*/
#include "internal.h"

/* ** The parser actor's whole mutable state: a back-pointer to the host parser
** the action drives. Everything heavy (CST, stacks, input) lives in the parser's
** own arena, so this block stays a single pointer -- the actor is coarse control
** around the VM, not a second home for parse state. */
typedef struct pp_parser_actor_data {
    pocketparse_parser *parser;
} pp_parser_actor_data;

/* ** Action for "actor.init": learn which host parser this actor drives. The
** parser pointer rides in the event payload; it is stored in the data block so
** the later end action can reach it. Runs once, right after the spawn. The
** payload is read with a byte copy rather than a pointer cast: ax_post lays the
** payload right after the event's name string, so its address carries the name's
** length as an offset and is not guaranteed to satisfy a pointer's alignment. */
static void pp_parser_actor_init(ax_actor *self, const ax_event *event)
{
    pp_parser_actor_data *data = (pp_parser_actor_data *)ax_data(self);
    if (event->data != 0 && event->len == sizeof(pocketparse_parser *)) {
        pocketparse_parser *parser;
        __builtin_memcpy(&parser, event->data, sizeof parser);
        data->parser = parser;
    }
}

/* ** Action for "input.end": run the bytecode VM over the buffered input inside
** this transition. The whole dispatch loop -- thousands of opcodes, the backtrack
** and tree-builder stacks, the memos -- is plain C here; ax does not see it. The
** result code is written to the parser so the host can read it after the system
** quiesces, and actor_ran is set so the host can prove the action genuinely ran.
** Entering "done" (an AX_FINAL state) on the same transition completes the actor. */
static void pp_parser_actor_end(ax_actor *self, const ax_event *event)
{
    pp_parser_actor_data *data = (pp_parser_actor_data *)ax_data(self);
    pocketparse_parser   *parser = data->parser;
    (void)event;
    if (parser == 0)
        return;
    parser->actor_ran    = 1;
    /* The tight loop runs here, in one action, to quiescence before it returns. */
    parser->actor_result = pp_execute_vm(parser);
}

/* ** Define the parser actor spec on the system. One running state accepts the
** init event and, on input.end, runs the VM and transitions to the AX_FINAL
** done state. Input bytes are buffered straight into the parser arena by push
** (see pp_parser_actor_push), so the actor sees only the init and end events,
** never the bytes -- nothing large ever flows through the system slice.
** Registered once per system; ax_define is idempotent only in that a second
** identical define returns the same registration, so the caller guards it (see
** pp_parser_actor_ensure). */
static ax_rc pp_define_parser_actor(ax_sys *system)
{
    const ax_spec spec = {
        "parser_actor",
        (const ax_state[]){
            { "running",
              AX_SIMPLE,
              0, 0, 0, 0,
              { 0, 0, 0 },
              0,
              (const ax_transition[]){
                  { "actor.init", 0,         0, pp_parser_actor_init,  0, 0, 0 },
                  { "input.end",  "done",    0, pp_parser_actor_end,   0, 0, 0 },
                  { 0, 0, 0, 0, 0, 0, 0 } },
              0, 0, 0 },
            { "done",
              AX_FINAL,
              0, 0, 0, 0,
              { 0, 0, 0 },
              0,
              (const ax_transition[]){ { 0, 0, 0, 0, 0, 0, 0 } },
              0, 0, 0 },
            { 0, AX_SIMPLE, 0, 0, 0, 0, { 0, 0, 0 }, 0, 0, 0, 0, 0 } },
        (const ax_transition[]){ { 0, 0, 0, 0, 0, 0, 0 } },
        (uint32_t)sizeof(pp_parser_actor_data),
        0
    };
    return ax_define(system, &spec);
}

/* ** Pump the system to quiescence: drain every runnable transition. One coarse
** drain after each posted event, exactly as axforest's grid_pump does -- the
** ready queue tracks the actor with work, so this returns as soon as the parser
** actor settles. */
static void pp_parser_actor_pump(ax_sys *system)
{
    while (ax_step(system) == AX_MORE) { }
}

/* ** Ensure the parser_actor spec is defined on the engine's system, defining it
** lazily the first time the actor path is used on this engine. The "defined"
** state is tracked PER ENGINE (engine->parser_actor_defined), not in a
** process-global static: a host that re-opens the engine -- the benchmark
** harness, an embedder running many parses -- gets a fresh ax system each open,
** and a fresh system has nothing defined on it. A process-global guard would
** skip the define on every system after the first and silently drop those parses
** off the actor path. ax_define returns AX_OK on the first call for a system;
** with the per-engine guard a second call never reaches it. Returns 1 when the
** spec is available. */
static int pp_parser_actor_ensure(pocketparse_engine *engine)
{
    if (engine == 0 || engine->system == 0)
        return 0;
    if (engine->parser_actor_defined)
        return 1;
    if (pp_define_parser_actor(engine->system) == AX_OK) {
        engine->parser_actor_defined = 1;
        return 1;
    }
    return 0;
}

/* ** Spawn the parser actor for a parser and tell it which host parser it drives.
** Called from pocketparse_parser_new on the actor path. The actor pid is stored
** on the parser so push/finish can address it. Pumps once so the init action
** runs before any chunk arrives. Returns 1 on success, 0 if the system or spawn
** is unavailable (the caller then falls back to the in-process VM). */
int pp_parser_actor_spawn(pocketparse_parser *parser)
{
    pocketparse_engine *engine;
    ax_pid              pid;
    ax_msg              message;
    pocketparse_parser *self_pointer;

    if (parser == 0 || parser->engine == 0)
        return 0;
    engine = parser->engine;
    if (!pp_parser_actor_ensure(engine))
        return 0;

    pid = ax_spawn(engine->system, "parser_actor");
    if (!ax_pid_ok(pid))
        return 0;
    parser->actor_pid = pid;

    self_pointer = parser;
    message.to             = pid;
    message.port           = 0;
    message.target         = 0;
    message.ev.name        = "actor.init";
    message.ev.origin      = (ax_pid){ 0, 0 };
    message.ev.via         = 0;
    message.ev.reply       = 0;
    message.ev.child       = 0;
    message.ev.data        = &self_pointer;
    message.ev.len         = (uint32_t)sizeof self_pointer;
    message.ev.correlation     = 0;
    message.ev.correlation_len = 0;
    message.delay          = 0;
    message.sendid         = 0;
    if (ax_post(engine->system, &message) != AX_OK)
        return 0;
    pp_parser_actor_pump(engine->system);
    return 1;
}

/* ** Buffer one chunk on the actor path. Called from pocketparse_push when the
** engine selects the actor-hosted VM and a parser actor is live. The bytes go
** straight into the parser's own arena (the same pp_parser_buffer the in-process
** path uses), NOT through an ax event: a payload posted to the actor would be
** copied into the fixed system slice, whose first-fit free list fragments across
** a reused engine until a file-sized allocation fails (the W60 corruption). The
** parser arena is the growable engine pool and is exactly where the input has
** always lived, so this faithfully keeps the actor copy-free while leaving the
** system slice untouched between parses. The parse still runs in the actor at
** input.end (see pp_execute_vm_actor). */
pocketparse_result pp_parser_actor_push(pocketparse_parser *parser,
                                        pocketparse_string bytes)
{
    if (parser == 0 || parser->engine == 0 || parser->engine->system == 0)
        return POCKETPARSE_INVALID;
    if (!ax_pid_ok(parser->actor_pid))
        return POCKETPARSE_INVALID;
    return pp_parser_buffer(parser, bytes);
}

/* ** Run the actor-hosted VM to completion: post input.end, pump the system to
** quiescence, and return the result the actor's action recorded. The action runs
** the whole VM dispatch loop inside one transition, so when this returns the
** parser's tree is built and frozen. Falls back to the in-process VM if the
** actor was never spawned (no system, or a spawn failure at parser_new) so the
** vm-ax flag never breaks a parse on a constrained grant. */
pocketparse_result pp_execute_vm_actor(pocketparse_parser *parser)
{
    pocketparse_engine *engine;
    ax_msg              message;

    if (parser == 0)
        return POCKETPARSE_INVALID;
    engine = parser->engine;

    /* No actor was spawned (small grant, or spawn failed): run the same VM in
    ** process. The tree is identical -- pp_execute_vm is the same function the
    ** action calls -- so the fallback is behaviourally transparent. */
    if (engine == 0 || engine->system == 0 || !ax_pid_ok(parser->actor_pid))
        return pp_execute_vm(parser);

    message.to             = parser->actor_pid;
    message.port           = 0;
    message.target         = 0;
    message.ev.name        = "input.end";
    message.ev.origin      = (ax_pid){ 0, 0 };
    message.ev.via         = 0;
    message.ev.reply       = 0;
    message.ev.child       = 0;
    message.ev.data        = 0;
    message.ev.len         = 0;
    message.ev.correlation     = 0;
    message.ev.correlation_len = 0;
    message.delay          = 0;
    message.sendid         = 0;
    if (ax_post(engine->system, &message) != AX_OK)
        return pp_execute_vm(parser);
    pp_parser_actor_pump(engine->system);

    /* The action must have run; if it somehow did not (a malformed spec), fall
    ** back so the parse still produces a tree rather than silently failing. */
    if (!parser->actor_ran)
        return pp_execute_vm(parser);
    return parser->actor_result;
}

/* ** Whether the parser actor's input.end action genuinely ran the dispatch loop
** for this parser. Reads the flag the action set; nonzero proves the parse
** executed inside the ax actor. */
int pp_parser_actor_ran(const pocketparse_parser *parser)
{
    return parser != 0 && parser->actor_ran;
}
