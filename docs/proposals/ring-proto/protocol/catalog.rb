# frozen_string_literal: true

# The ring protocol's records: the single definition every document and
# renderer reads. Evaluated by RingProto.load (dsl.rb).
#
# Identifiers are stable: `<ring>.<name>` and the 16-bit code never change
# meaning once a scenario cites them. A retired message keeps its code
# reserved.
#
# Codes: 0x01xx host to core (h2c), 0x02xx core to host (c2h),
# 0x03xx telemetry (tel). Code 0x0000 is reserved for the pad record a
# producer writes when a record would not fit before the wrap
# (reference/ring.md, "The wrap").

title 'Ring protocol records'

note 'The data model is not on a ring. Core calls ta_datamodel_vtable ' \
     'synchronously, as today (reference/data-model.md). Only its results ' \
     'cross the rings, as value references.'
note 'Definition management uses request/result records. Native calls create ' \
     'and destroy the system, expose its rings, drive execution and install ' \
     'data model/value integrations. An attached debugger takes exclusive ' \
     'ownership of the whole system from the production pump. Inspection, ' \
     'abandonment and operator transitions use native primitives outside ' \
     'the production rings; no operator ring is added ' \
     '(reference/debugging.md).'
note 'A vref in a c2h record is a reference core retained for the host. ' \
     'The host releases it when it has used the record. A vref in an h2c ' \
     'record transfers one retained reference to core at commit. Core ' \
     'releases it on refusal, drop or completion; a bounce returns status, ' \
     'not ownership. Host retry originals belong in a bounded submission table.'
note 'Replay seeds new execution: supplied inputs derive configuration and ' \
     'data model values, which may differ from an earlier run. External ' \
     'effects are suppressed while seeding. In live execution the host may ' \
     'execute effects on receipt, independently of recording, persistence ' \
     'or c2h.macrostep_ended.'

# ---------------------------------------------------------------- rings

ring :h2c,
     producer: :host, consumer: :core, loss: :none,
     when_full: 'The host cannot post. It refuses at its edge (for example ' \
                'HTTP 503) or keeps the input itself. Core never waits on h2c.',
     doc: 'Host requests, in host order. An event that cannot be admitted ' \
          'is consumed and bounced after its refusal output is reserved. ' \
          'Admission is not an execution barrier. Definition requests run between ' \
          'macrosteps without an envelope (reference/definitions.md).'

ring :c2h,
     producer: :core, consumer: :host, loss: :none,
     when_full: 'A record an action would write does not fit: the action fails ' \
                'as error.execution.capacity. Core starts a macrostep only when ' \
                'c2h has room for all mandatory observations, cancellations, ' \
                'autoforwards, completion and end reports, including wrap padding. ' \
                'Startup and control phases can yield between macrosteps.',
     doc: 'Everything that leaves the process or must not be lost: effects for ' \
          'I/O processors and invokers, macrostep end reports, bounces, load requests, ' \
          'images, and the inputs of recorded sessions.'

ring :tel,
     producer: :core, consumer: :host, loss: :drop,
     when_full: 'The record is dropped and counted. The next tel.dropped says ' \
                'how many. Telemetry never slows the system.',
     doc: 'Observability: the trace records of core ADR 0067 under the ' \
          'existing kind masks and sampling. Never used for replay.'

# ---------------------------------------------------------------- enums

enum :event_type, :u8, doc: 'What _event.type says (SCXML 5.10.1).' do
  value :platform, 0
  value :internal, 1
  value :external, 2
end

enum :input_source, :u8, doc: 'Where the envelope behind a c2h.input came from.' do
  value :h2c, 0, 'a host input; source_seq is its h2c seq'
  value :mail, 1, 'a <send> from another local session; from_session names it'
  value :timer, 2, 'a delayed send that became due; from_session sent it'
  value :platform, 3, 'core made it: error.communication for undeliverable mail, done.invoke, ...'
  value :invoke_report, 4, 'an h2c.invoke_report'
end

enum :macrostep_status, :u8, doc: 'How a macrostep ended.' do
  value :stable, 0, 'stabilization ended; invocation startup and internal follow-up may still follow'
  value :done, 1, 'the session reached a top-level final state; c2h.finished follows'
  value :runaway, 2, 'the microstep limit stopped it (core ADR 0075); the session is stable'
  value :abandoned, 3, 'an operator abandoned it (core ADR 0082)'
  # Value 4 was the unapproved terminal-capacity sketch; reserved, not emitted.
end

enum :input_completion_status, :u8, doc: 'How an input and its invocation-startup follow-up settled.' do
  value :stable, 0, 'ready for another external input'
  value :done, 1, 'the session finished'
  value :runaway, 2, 'execution stopped at its configured limit'
  value :abandoned, 3, 'an operator abandoned execution'
end

enum :input_refusal_reason, :u8, doc: 'Why an admitted host input was discarded before take.' do
  value :load_failed, 1
  value :destroyed, 2
  value :snapshot_failed, 3, 'copy_out could not produce the recording snapshot'
  value :stale_invocation, 4, 'the service instance is no longer active'
  value :invalid, 5, 'validation at take failed, for example a backwards replay clock'
  value :withdrawn, 6, 'h2c.withdraw named it before it was taken'
end

enum :bounce_reason, :u8, doc: 'Why core handed an h2c record back unrun.' do
  value :no_such_address, 1, 'no session has the address and its family has no loader'
  value :no_such_definition, 2, 'the revision is not registered'
  value :address_taken, 3, 'h2c.spawn for an address that is resident'
  value :has_mail, 4, 'h2c.unload for a session with envelopes waiting'
  value :not_replaying, 5, 'h2c.publish for a session that is not replaying'
  value :invalid, 6, 'the record breaks the protocol; detail says how'
  value :pool, 7, 'admission watermark, envelope quota or load restoration capacity'
  value :names, 8, 'admission watermark or insufficient name-area storage'
  value :allocation, 9, 'allocation outside macrosteps failed without committing the operation'
  value :active_local_invoke, 10, 'unload of a local invocation owner or active invoked child'
  value :pending_inputs, 11, 'publish attempted before supplied replay inputs settled'
  value :stale_request, 12, 'a response does not match the outstanding load attempt'
  value :record, 13, 'mandatory output can never fit the configured record or ring bounds'
end

enum :capacity_resource, :u8,
     doc: 'The last segment of error.execution.capacity.<resource>, the event an ' \
          'action raises when a bounded resource has no room for it. Its data names ' \
          'the action (reference/macrostep.md, "Two kinds of exhaustion").' do
  value :pool, 1, 'no free envelope for mail or a delayed send (poolEnvelopes)'
  value :names, 2, 'no room in the name area for a run-time name (nameAreaBytes)'
  value :c2h, 3, 'no room on c2h for the record the action writes (c2hBytes)'
  value :internal, 4, 'the internal queue is full; <raise> failed'
  value :record, 5, 'the record would be larger than maxRecordBytes'
end

enum :invoke_report_kind, :u8 do
  value :done, 0, 'the invoked service finished: done.invoke.<id>'
  value :error, 1, 'it failed: error.communication.invoke.<id>.<reason> (core ADR 0081)'
end

enum :destroy_reason, :u8 do
  value :finished, 0
  value :operator, 1, 'h2c.destroy'
  value :parent_exited, 2, 'its invoking state exited'
end

enum :definition_status, :u8, doc: 'The result of a definition request.' do
  value :ok, 0
  value :compile_error, 1, 'the complete spec was refused; diagnostic describes the first fault'
  value :capacity, 2, 'allocation, a configured bound or diagnostic buffer capacity was exhausted'
  value :mismatch, 3, 'an explicit identity names different content (core ADR 0077)'
  value :not_found, 4, 'an unknown or stale definition ID'
  value :invalid, 5, 'an invalid spec representation, adapter handle or release request'
end

# ---------------------------------------------------------------- h2c

message 'h2c.time', code: 0x0101, summary: 'Move the system clock' do
  doc 'The header `at` is the new time. It never moves back. Delayed ' \
      'sends due by then become runnable envelopes, in due order, then ' \
      'send order. Applied between macrosteps. Admission of an earlier event ' \
      'does not ensure that event has finished; the host awaits input_completed ' \
      'before posting time when it requires that execution order.'
  replies_with 'c2h.wake'
end

message 'h2c.event', code: 0x0102, summary: 'An external event for a session' do
  doc 'Header `session` names a resident session, or is 0 and `target` ' \
      'gives an address. Admitted into the pool when the pool is below its ' \
      'watermark. An event for an unloaded address waits in the pool and ' \
      'core asks for the session with c2h.load_needed. A capacity refusal ' \
      'consumes and bounces the record, allowing later controls to progress.'
  doc 'A live input runs at system time when taken; its header at does not ' \
      'advance that clock. A replay input moves its prepared session clock ' \
      'at take. Complete event metadata must also fit the expanded c2h.input. ' \
      'Service events carry invocation_seq, checked again at take. Zero ' \
      'means ordinary input, or an application-supplied replay seed.'
  field :target, :text, 'the address (family/name), when session is 0'
  field :name, :text, 'the dotted event name, as bytes (reference/names.md)'
  field :sendid, :text, '_event.sendid, or empty'
  field :origin, :text, '_event.origin, or empty'
  field :origin_type, :text, '_event.origintype, or empty'
  field :invokeid, :text, '_event.invokeid for an invoked service, or empty'
  field :invocation_seq, :seq, 'c2h.invoke seq for a live service instance; 0 for ordinary input or replay seeds'
  field :data, :vref, '_event.data'
  field :type, :event_type, 'almost always external'
  replies_with 'c2h.input', 'c2h.bounced', 'c2h.load_needed', 'c2h.input_refused', 'c2h.input_completed'
  see 'h2c.withdraw'
end

message 'h2c.spawn', code: 0x0103, summary: 'Create a session' do
  doc 'With the replaying flag the session is prepared to replay (core ' \
      'ADR 0089): its clock starts at `clock`, it crosses no border, and ' \
      'it is published with h2c.publish. Inputs seed new configuration ' \
      'and data model values; exact restoration is not promised.'
  doc 'With `state`, the session is restored from a checkpoint (c2h.state, ' \
      'core ADR 0091) as a replaying session whose clock starts at the ' \
      "state's; the c2h.input records after the checkpoint follow. This is " \
      'how a session survives a restart. h2c.load is not: it only pages ' \
      'a session back into memory within one process.'
  flag :replaying_from_clock, 0, 'the session replays; `clock` is where its run began'
  field :address, :text, 'family/name'
  field :revision, :bytes, "the definition's revision hash"
  field :data, :vref, 'the data model binding, or 0'
  field :clock, :time, 'with replaying_from_clock: the replayed run’s start'
  field :state, :blob, 'a checkpoint c2h.state wrote, to restore; 0 for none'
  field :request_seq, :seq, 'c2h.load_needed seq when answering a virtual-family load; 0 for an unsolicited spawn'
  replies_with 'c2h.spawned', 'c2h.bounced', 'c2h.input_completed'
end

message 'h2c.publish', code: 0x0104, summary: 'End a replay and put the session on the route' do
  doc 'Held delayed sends due by `at` are dropped; the rest stay in the ' \
      'pool under their due times (core ADR 0094). Active invocations are ' \
      'retired with error.communication.invoke.<id>.unavailable.'
  doc 'An empty `address` publishes every replaying session at once: the ' \
      'end of a whole-system replay (reference/recording-and-replay.md).'
  doc 'Refuses without mutation while supplied seed inputs remain pending. ' \
      'Generated local mail need not drain: pending mail follows the selected ' \
      'publication cut into live execution. Publication does not execute effects ' \
      'suppressed during seeding.'
  field :address, :text, 'family/name, or empty for every replaying session'
  field :at, :time, 'the time chosen for the seeded session'
  replies_with 'c2h.published', 'c2h.bounced'
end

message 'h2c.load', code: 0x0105, summary: 'Bring an unloaded session back' do
  doc 'Usually the answer to c2h.load_needed. Its delayed sends rejoin the ' \
      'pool. Waiting envelopes for it become runnable.'
  doc 'Loading only bounds memory; it is not persistence. The image is one ' \
      'c2h.unloaded wrote in this process, and it is valid only here ' \
      '(core ADR 0074). To restore a session after a restart, use ' \
      'h2c.spawn with `state` (reference/unloading.md).'
  field :address, :text
  field :image, :blob, 'the bytes c2h.unloaded gave, in this process'
  field :request_seq, :seq, 'c2h.load_needed seq; 0 for an unsolicited timer-driven load'
  doc 'Stage the complete restore outside macrosteps. Capacity or decoding ' \
      'failure leaves residency, timers and waiting mail unchanged. The host ' \
      'pins image until c2h.loaded or c2h.bounced correlates this request. ' \
      'Zero request_seq is refused when an active load request exists for the address.'
  replies_with 'c2h.loaded', 'c2h.bounced'
end

message 'h2c.load_failed', code: 0x0106, summary: 'The session cannot be loaded' do
  doc 'Every envelope waiting for the address is recycled: mail becomes ' \
      'error.communication for its sender, in the same envelope ' \
      '(reference/envelope.md, "Recycling"); external inputs receive ' \
      'c2h.input_refused and DEAD_LETTER telemetry. Large waiting lists ' \
      'are processed incrementally between macrosteps as c2h space permits.'
  field :address, :text
  field :reason, :text
  field :request_seq, :seq, 'the outstanding c2h.load_needed seq; a stale attempt changes nothing'
  replies_with 'c2h.load_result', 'c2h.input_refused', 'c2h.bounced'
end

message 'h2c.unload', code: 0x0107, summary: 'Page a stable session out' do
  field :address, :text
  field :image, :blob, 'host-provided writable sink, pinned until unloaded or bounced'
  doc 'Refuses an owner of an active local invocation and an active locally ' \
      'invoked child. No lifetime envelope token moves into an image.'
  replies_with 'c2h.unloaded', 'c2h.bounced'
end

message 'h2c.checkpoint', code: 0x0108, summary: "Ask for a recorded session's state" do
  field :address, :text
  field :image, :blob, 'host-provided writable sink, pinned until state or bounced'
  replies_with 'c2h.state', 'c2h.bounced'
  see 'h2c.spawn'
end

message 'h2c.invoke_report', code: 0x0109, summary: 'An invoked service finished or failed' do
  doc 'Header `session` is the invoking session.'
  field :invokeid, :text
  field :invocation_seq, :seq, 'the c2h.invoke seq; checked at take, 0 only for replay seeds'
  field :reason, :text, 'for error: the reason segment'
  field :data, :vref, 'done data or error data'
  field :kind, :invoke_report_kind
  field :ends, :bool, 'for error: retire the matching instance at take after validation and internal notification enqueue; false preserves it'
  doc 'Done enters the external queue with invoke metadata for finalize; ' \
      'errors are platform/internal events without event invokeid. A stale ' \
      'live instance cannot change a later invocation with the same authored ID.'
  replies_with 'c2h.input', 'c2h.bounced', 'c2h.input_refused', 'c2h.input_completed'
  see 'h2c.withdraw'
end

message 'h2c.destroy', code: 0x010A, summary: 'End a session now' do
  field :address, :text
  replies_with 'c2h.destroyed', 'c2h.bounced'
end

message 'h2c.definition_register', code: 0x010B, summary: 'Compile and register a complete definition' do
  doc 'Header session is 0. One atomic request, with no builder operations. ' \
      'Core validates a relocatable ta_scxml_spec, compiles it through the ' \
      'existing compiler and registers it between macrosteps. Native data ' \
      'model callbacks remain synchronous (reference/definitions.md).'
  doc 'The host keeps spec readable and diagnostic writable until the ' \
      'correlated c2h.definition_result. Failure registers nothing and ' \
      'retains nothing from the submitted spec.'
  field :spec, :blob, 'complete declarative spec; offsets and counts, not native pointers'
  field :datamodel, :u64, 'process-local handle of a native-installed data model adapter'
  field :diagnostic, :blob, 'host-owned bounded output buffer for a structured compile error'
  replies_with 'c2h.definition_result'
end

message 'h2c.definition_release', code: 0x010C, summary: 'Release one registration hold' do
  doc 'Core ADR 0077 semantics: an unused definition is freed when its last ' \
      'hold, actor and nesting reference goes. Header session is 0.'
  field :definition, :u64, 'the system-local definition ID returned by registration'
  replies_with 'c2h.definition_result'
end

message 'h2c.definition_retire', code: 0x010D, summary: 'End a definition’s availability' do
  doc 'Core ADR 0043 semantics: existing actors keep running; new spawns ' \
      'cannot use it. Retirement drops registration holds and is idempotent ' \
      'for an issued ID, including after it was freed. Header session is 0.'
  field :definition, :u64, 'the system-local definition ID'
  replies_with 'c2h.definition_result'
end

message 'h2c.withdraw', code: 0x010E, summary: 'Take back an admitted input that has not been taken' do
  doc 'Header session is 0. A control: it takes no envelope and is applied ' \
      'when read, also while core is not admitting. target_seq names an ' \
      'earlier h2c.event or h2c.invoke_report. If that input waits in the ' \
      'pool (runnable or on a waiting list), core unlinks and frees its ' \
      'envelope, releases its transferred values and name text, and writes ' \
      'c2h.input_refused with reason withdrawn. Core reserves that record ' \
      'before consuming the withdrawal (reference/envelope.md).'
  doc 'If the input is no longer waiting, the withdrawal changes nothing ' \
      'and writes nothing: the input already has, or will get, its own ' \
      'single terminal record (c2h.bounced, c2h.input_refused or ' \
      'c2h.input_completed). A taken input, including one paused inside ' \
      'its macrostep, is never withdrawn; stopping it is an operator ' \
      'abandon (reference/debugging.md). A target_seq not lower than this ' \
      'record\'s own seq is bounced as invalid.'
  field :target_seq, :seq, 'the h2c seq of the input to withdraw'
  replies_with 'c2h.input_refused', 'c2h.bounced'
end

# ---------------------------------------------------------------- c2h

message 'c2h.spawned', code: 0x0201, summary: 'A session exists' do
  doc 'For a recorded session this is the first record of its log.'
  doc 'Creation does not imply bootstrap and invocation startup have settled. ' \
      'c2h.input_completed correlates that completion with the originating spawn.'
  field :address, :text
  field :revision, :bytes
  field :parent, :session, 'the invoking session, or 0'
  field :cause_seq, :seq, 'the h2c.spawn, or 0 for an <invoke>'
end

message 'c2h.input', code: 0x0202, summary: 'A recorded session took an input' do
  doc 'Written when a recorded session takes an envelope, before any ' \
      'effect of the macrostep it starts. These observations, ' \
      'fed back as h2c.event and h2c.invoke_report under ' \
      'h2c.spawn(replaying), seed a new execution. Recording imposes no ' \
      'persistence-before-effects rule.'
  doc 'Not written for an input a replaying session takes: that input is ' \
      'already in the log being replayed. A session restored from c2h.state ' \
      'and replayed starts writing c2h.input again after h2c.publish.'
  doc 'Data is a copy_out snapshot created before binding or running the input, ' \
      'including finalize and autoforward. Later chart mutations cannot change it.'
  field :macrostep, :macrostep
  field :source_seq, :seq, 'source h2c: the h2c seq'
  field :from_session, :session, 'source mail or timer: the sender'
  field :name, :text
  field :sendid, :text
  field :origin, :text
  field :origin_type, :text
  field :invokeid, :text
  field :data, :vref
  field :source, :input_source
  field :type, :event_type
  field :report_invokeid, :text, 'source invoke_report: original report invocation ID, separate from event invokeid'
  field :report_reason, :text, 'source invoke_report/error: original reason token'
  field :report_kind, :invoke_report_kind, 'source invoke_report only; otherwise ignored'
  field :report_ends, :bool, 'source invoke_report/error: original retirement decision'
end

message 'c2h.send', code: 0x0203, summary: 'A send for an I/O processor' do
  doc 'Mail to a local session never appears here: it is an envelope ' \
      '(reference/envelope.md). A delayed send to an I/O processor waits ' \
      'in the pool and is written when due. The host may execute the send ' \
      'on receipt, without waiting for c2h.macrostep_ended or persistence.'
  doc 'A delayed send that becomes due on h2c.time is written outside any ' \
      'macrostep: its `macrostep` is 0, no end report follows, and the host may ' \
      'act on it at once. An end report would describe a macrostep ' \
      'that does not exist (reference/envelope.md, "A due send to an ' \
      'I/O processor has no macrostep end").'
  field :macrostep, :macrostep, '0 for a delayed send that became due'
  field :sendid, :text
  field :io_type, :text
  field :target, :text
  field :name, :text
  field :data, :vref, 'the namelist and params, aggregated by the data model'
  field :content, :vref, '<content>, or 0'
  see 'c2h.macrostep_ended'
end

message 'c2h.invoke', code: 0x0204, summary: 'Start an invocation' do
  doc 'Written in the post-macrostep startup phase. This record seq is the ' \
      'live service-instance token; authored invokeid remains the SCXML identity.'
  field :macrostep, :macrostep, 'the stabilization macrostep that enabled this startup'
  field :invokeid, :text
  field :type, :text
  field :src, :text
  field :data, :vref
  field :content, :vref, 'evaluated content, or 0'
  field :autoforward, :bool
  replies_with 'h2c.invoke_report', 'h2c.event'
end

message 'c2h.invoke_cancel', code: 0x0205, summary: 'Cancel an invocation' do
  field :macrostep, :macrostep
  field :invokeid, :text
  field :invocation_seq, :seq, 'the canceled c2h.invoke instance'
  doc 'Mandatory state-exit output, reserved before execution. The host ' \
      'invalidates this instance and ignores its late callbacks.'
end

message 'c2h.invoke_forward', code: 0x0206, summary: 'Autoforward an event to an invocation' do
  field :macrostep, :macrostep
  field :invokeid, :text, 'destination invocation, not the forwarded event invokeid'
  field :invocation_seq, :seq, 'destination c2h.invoke instance'
  field :name, :text
  field :sendid, :text
  field :origin, :text
  field :origin_type, :text, 'the original event origintype'
  field :event_invokeid, :text, 'the original event invokeid'
  field :data, :vref
  field :type, :event_type
  doc 'Every event field is preserved at the autoforward point. Finalize runs ' \
      'first, as in the SCXML algorithm; each recipient then receives its own ' \
      'copy_out data before transition code runs. ' \
      'Record space is mandatory; a data-model copy failure remains error.execution.'
end

message 'c2h.log', code: 0x0207, summary: 'A <log>' do
  field :macrostep, :macrostep
  field :label, :text
  field :value, :vref
end

message 'c2h.macrostep_ended', code: 0x0208, summary: 'A macrostep ended' do
  doc 'Everything the macrostep wrote to c2h comes before this record. The ' \
      'host may already have executed effects on receipt. This record ' \
      'reports that execution ended and gives its status, including ' \
      'abandonment; invocation startup may follow. It is neither input completion nor ' \
      'a durability acknowledgement (reference/macrostep.md, "The macrostep end").'
  field :macrostep, :macrostep
  field :source, :input_source
  field :source_seq, :seq, 'originating h2c input, including internal startup follow-up; 0 for local work'
  field :effects, :u32, 'records this macrostep wrote to c2h, this end report excluded'
  field :mail, :u32, 'envelopes it put in the pool'
  field :status, :macrostep_status
end

message 'c2h.wake', code: 0x0209, summary: 'Call core back at this time' do
  doc 'The earliest due delayed send in the pool changed. One timer in the ' \
      "host's event loop is enough."
  flag :none, 0, 'nothing is due: cancel the timer'
  field :due, :time
  replies_with 'h2c.time'
end

message 'c2h.bounced', code: 0x020A, summary: 'An h2c record was not run' do
  doc 'Core consumes the record, releases transferred values and keeps nothing ' \
      'of it. The host decides whether to retry using its own retained original. ' \
      'Reserve the bounded reply before consuming the request. This is terminal ' \
      'before admission; an admitted input instead ends in refused or completed.'
  field :source_seq, :seq
  field :detail, :text
  field :reason, :bounce_reason
end

message 'c2h.load_needed', code: 0x020B, summary: 'Envelopes wait for an unloaded address' do
  doc 'Written once per address until h2c.load or h2c.load_failed answers it.'
  field :address, :text
  field :waiting, :u32, 'envelopes waiting for it'
  doc 'For a family whose first message creates the actor, h2c.spawn answers it.'
  replies_with 'h2c.load', 'h2c.spawn', 'h2c.load_failed'
end

message 'c2h.loaded', code: 0x020C, summary: 'A session is resident again' do
  field :address, :text
  field :cause_seq, :seq, 'the h2c.load'
end

message 'c2h.unloaded', code: 0x020D, summary: 'A session left memory' do
  doc "The image holds the session's state and its delayed sends. The " \
      'host keeps it, and wake_at, in its own index of unloaded sessions, ' \
      'and posts h2c.load when wake_at comes.'
  doc 'The image bounds memory; it is not a checkpoint. It is valid only in ' \
      'the process that wrote it and ends with it. c2h.state is the ' \
      'checkpoint.'
  flag :has_wake, 0, 'wake_at is meaningful'
  field :address, :text
  field :cause_seq, :seq, 'the h2c.unload'
  field :image, :blob
  field :image_bytes, :u64, 'complete valid bytes written into the requested sink'
  field :wake_at, :time, 'its earliest delayed send'
end

message 'c2h.state', code: 0x020E, summary: "A recorded session's state at a point of its log" do
  doc 'A checkpoint (core ADR 0091). Its delayed sends are inside the ' \
      'image, so it is one artifact. Replaying from it needs only the ' \
      'c2h.input records after input_seq.'
  doc 'Unlike the image c2h.unloaded writes, it is meant to be stored: its ' \
      'values are written with value_write durable=true (core ADR 0090). ' \
      'It is restored with h2c.spawn `state`.'
  field :address, :text
  field :cause_seq, :seq, 'the h2c.checkpoint'
  field :image, :blob
  field :image_bytes, :u64, 'complete valid bytes written into the requested sink'
  field :input_seq, :seq, 'the last c2h.input it includes'
end

message 'c2h.published', code: 0x020F, summary: 'A replayed session is live' do
  doc 'For an empty-address publication, one session-0 result summarizes the ' \
      'atomic mode change for all replaying sessions.'
  field :address, :text
  field :cause_seq, :seq, 'the h2c.publish'
  field :replayed, :u64, 'inputs it replayed; total for all-session publication'
  field :dropped_timers, :u64, 'held delayed sends due by `at`'
  field :retired_invokes, :u64
end

message 'c2h.finished', code: 0x0210, summary: 'A session reached a top-level final state' do
  doc "Mail to a local parent (done.invoke) is an envelope; this record is for the host's books."
  field :macrostep, :macrostep
  field :donedata, :vref
end

message 'c2h.destroyed', code: 0x0211, summary: 'A session is gone' do
  doc 'Envelopes still waiting for it are recycled as for h2c.load_failed.'
  field :address, :text
  field :cause_seq, :seq, 'the h2c.destroy, or 0 for execution-driven destruction'
  field :reason, :destroy_reason
end

message 'c2h.definition_result', code: 0x0212, summary: 'A definition request completed' do
  doc 'Exactly one result per definition request. Header session is 0. ' \
      'Core reserves room for this fixed-size reply before consuming the ' \
      'request or changing definition state. No macrostep end report follows.'
  doc 'A compile_error diagnostic owns copies of the code, path, subject, ' \
      'subject range, related text and message of ta_compile_error (core ' \
      'ADR 0058); no borrowed pointers escape. The host owns the buffer ' \
      'before and after completion (reference/definitions.md).'
  field :cause_seq, :seq, 'the definition request’s h2c seq'
  field :definition, :u64, 'on successful register: the definition ID; otherwise 0'
  field :diagnostic_bytes, :u32, 'bytes written to the request’s diagnostic buffer; 0 without a compile error'
  field :status, :definition_status
end

message 'c2h.input_refused', code: 0x0213, summary: 'An admitted host input was not taken' do
  doc 'Exactly one terminal refusal for an admitted h2c.event or invoke_report ' \
      'discarded by load failure, destruction, stale-instance validation, ' \
      'snapshot failure or an h2c.withdraw. No input observation or completion follows. Core ' \
      'releases its transferred values; host retry originals remain host-owned.'
  field :source_seq, :seq, 'the originating h2c input'
  field :reason, :input_refusal_reason
end

message 'c2h.input_completed', code: 0x0214, summary: 'A host input and its startup follow-up settled' do
  doc 'Exactly one terminal result for a taken host input, including spawn ' \
      'bootstrap. Emitted after stabilization and invocation-startup internal ' \
      'follow-up settle, regardless of recording or replaying flags. Earlier ' \
      'macrostep ends do not settle the host operation. This does not hold ' \
      'effects, await child-service completion or acknowledge persistence.'
  field :source_seq, :seq, 'the originating h2c input'
  field :macrostep, :macrostep, 'the last macrostep in the input execution'
  field :status, :input_completion_status
end

message 'c2h.load_result', code: 0x0215, summary: 'A load-failure response finished recycling waiting inputs' do
  doc 'Acknowledges h2c.load_failed after all waiting envelopes were recycled ' \
      'or refused. Each external input refusal precedes this result. Recycling ' \
      'may yield between macrosteps to drain c2h; its cursor uses the existing ' \
      'waiting list. A stale attempt receives bounced instead.'
  field :cause_seq, :seq, 'the h2c.load_failed'
  field :request_seq, :seq, 'the answered c2h.load_needed'
end

# ---------------------------------------------------------------- tel

message 'tel.trace', code: 0x0301, summary: 'One trace record (core ADR 0067)' do
  doc 'The fields of ta_trace_record that its kind names. Values are ' \
      'text the data model renders (value_text), cut at the parameter ' \
      'telemetryValueBytes; vrefs never go on a lossy ring.'
  flag :truncated, 0, 'value_text was cut'
  flag :suppressed, 1, 'an effect a replaying session did not perform'
  field :macrostep, :macrostep
  field :trace_id_high, :u64
  field :trace_id_low, :u64
  field :cause_session, :session
  field :cause_macrostep, :macrostep
  field :microstep, :u32
  field :state, :u32
  field :transition, :u32
  field :action, :u32
  field :iteration, :u32
  field :trace_kind, :u16, 'ta_trace_kind'
  field :position, :u8, 'ta_trace_at'
  field :result, :bool
  field :name, :text
  field :value_text, :text
  field :states, :bytes, 'u32 state IDs: exited, entered, history, counted in the tail'
end

message 'tel.dropped', code: 0x0302, summary: 'Telemetry records were dropped' do
  field :count, :u64
  field :first_seq, :seq, 'the seq the first dropped record would have had'
end
