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
note 'Defining a chart, creating a system and the operator calls (inspect, ' \
     'abandon, operator transitions) stay ordinary API calls: they are ' \
     'control, not traffic (ARCHITECTURE.md, "Two kinds of call").'
note 'A vref in a c2h record is a reference core retained for the host. ' \
     'The host releases it when it has used the record. A vref in an h2c ' \
     'record is one the host retained for core; core releases it when the ' \
     'envelope holding it is freed.'

# ---------------------------------------------------------------- rings

ring :h2c,
     producer: :host, consumer: :core, loss: :none,
     when_full: 'The host cannot post. It refuses at its edge (for example ' \
                'HTTP 503) or keeps the input itself. Core never waits on h2c.',
     doc: 'Admission. Every input the system will ever see enters here, in ' \
          'the host order. Core moves a record into the envelope pool only ' \
          'while the pool is below its admission watermark.'

ring :c2h,
     producer: :core, consumer: :host, loss: :none,
     when_full: 'A record an action would write does not fit: the action fails ' \
                'as error.execution.capacity. Core starts a macrostep only when ' \
                'c2h has room for its reserve (c2h.input, c2h.fence and one ' \
                'record of the largest size). A run never waits.',
     doc: 'Everything that leaves the process or must not be lost: effects for ' \
          'I/O processors and invokers, fences, bounces, load requests, ' \
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
  value :h2c, 0, 'an h2c.event; source_seq is its h2c seq'
  value :mail, 1, 'a <send> from another local session; from_session names it'
  value :timer, 2, 'a delayed send that became due; from_session sent it'
  value :platform, 3, 'core made it: error.communication for undeliverable mail, done.invoke, ...'
  value :invoke_report, 4, 'an h2c.invoke_report'
end

enum :fence_status, :u8, doc: 'How a macrostep ended.' do
  value :stable, 0, 'the session waits for its next input'
  value :done, 1, 'the session reached a top-level final state; c2h.finished follows'
  value :runaway, 2, 'the microstep limit stopped it (core ADR 0075); the session is stable'
  value :abandoned, 3, 'an operator abandoned it (core ADR 0082)'
  value :capacity, 4, "a microstep's reserve could not be restored; the macrostep ended there " \
                      '(reference/envelope.md, "Running out"); a tel.trace says why'
end

enum :bounce_reason, :u8, doc: 'Why core handed an h2c record back unrun.' do
  value :no_such_address, 1, 'no session has the address and its family has no loader'
  value :no_such_definition, 2, 'the revision is not registered'
  value :address_taken, 3, 'h2c.spawn for an address that is resident'
  value :has_mail, 4, 'h2c.unload for a session with envelopes waiting'
  value :not_replaying, 5, 'h2c.publish for a session that is not replaying'
  value :invalid, 6, 'the record breaks the protocol; detail says how'
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

# ---------------------------------------------------------------- h2c

message 'h2c.time', code: 0x0101, summary: 'Move the system clock' do
  doc 'The header `at` is the new time. It never moves back. Delayed ' \
      'sends due by then become runnable envelopes, in due order, then ' \
      'send order.'
  replies_with 'c2h.wake'
end

message 'h2c.event', code: 0x0102, summary: 'An external event for a session' do
  doc 'Header `session` names a resident session, or is 0 and `target` ' \
      'gives an address. Admitted into the pool when the pool is below its ' \
      'watermark. An event for an unloaded address waits in the pool and ' \
      'core asks for the session with c2h.load_needed.'
  field :target, :text, 'the address (family/name), when session is 0'
  field :name, :text, 'the dotted event name, as bytes (reference/names.md)'
  field :sendid, :text, '_event.sendid, or empty'
  field :origin, :text, '_event.origin, or empty'
  field :origin_type, :text, '_event.origintype, or empty'
  field :data, :vref, '_event.data'
  field :type, :event_type, 'almost always external'
  replies_with 'c2h.input', 'c2h.bounced', 'c2h.load_needed'
end

message 'h2c.spawn', code: 0x0103, summary: 'Create a session' do
  doc 'With the replaying flag the session is prepared to replay (core ' \
      'ADR 0089): its clock starts at `clock`, it crosses no border, and ' \
      'it is published with h2c.publish.'
  flag :replaying_from_clock, 0, 'the session replays; `clock` is where its run began'
  field :address, :text, 'family/name'
  field :revision, :bytes, "the definition's revision hash"
  field :data, :vref, 'the data model binding, or 0'
  field :clock, :time, 'with replaying_from_clock: the replayed run’s start'
  replies_with 'c2h.spawned', 'c2h.bounced'
end

message 'h2c.publish', code: 0x0104, summary: 'End a replay and put the session on the route' do
  doc 'Held delayed sends due by `at` are dropped; the rest stay in the ' \
      'pool under their due times (core ADR 0094). Active invocations are ' \
      'retired with error.communication.invoke.<id>.unavailable.'
  doc 'An empty `address` publishes every replaying session at once: the ' \
      'end of a whole-system replay (reference/recording-and-replay.md).'
  field :address, :text, 'family/name, or empty for every replaying session'
  field :at, :time, 'the moment the replayed run stopped'
  replies_with 'c2h.published', 'c2h.bounced'
end

message 'h2c.load', code: 0x0105, summary: 'Bring an unloaded session back' do
  doc 'Usually the answer to c2h.load_needed. Its delayed sends rejoin the ' \
      'pool. Waiting envelopes for it become runnable.'
  doc 'With replaying, the image is a checkpoint (c2h.state): the session ' \
      'replays the c2h.input records after it and is published with ' \
      'h2c.publish, as a spawned replaying session is.'
  flag :replaying, 0, 'restore a checkpoint as a replaying session'
  field :address, :text
  field :image, :blob, 'the bytes c2h.unloaded gave'
  replies_with 'c2h.loaded', 'c2h.bounced'
end

message 'h2c.load_failed', code: 0x0106, summary: 'The session cannot be loaded' do
  doc 'Every envelope waiting for the address is recycled: mail becomes ' \
      'error.communication for its sender, in the same envelope ' \
      '(reference/envelope.md, "Recycling"); external events are dropped ' \
      'and reported as tel.trace with a new trace kind, DEAD_LETTER, ' \
      'naming the event and the reason.'
  field :address, :text
  field :reason, :text
end

message 'h2c.unload', code: 0x0107, summary: 'Page a stable session out' do
  field :address, :text
  replies_with 'c2h.unloaded', 'c2h.bounced'
end

message 'h2c.checkpoint', code: 0x0108, summary: "Ask for a recorded session's state" do
  field :address, :text
  replies_with 'c2h.state', 'c2h.bounced'
end

message 'h2c.invoke_report', code: 0x0109, summary: 'An invoked service finished or failed' do
  doc 'Header `session` is the invoking session.'
  field :invokeid, :text
  field :reason, :text, 'for error: the reason segment'
  field :data, :vref, 'done data or error data'
  field :kind, :invoke_report_kind
  replies_with 'c2h.input'
end

message 'h2c.destroy', code: 0x010A, summary: 'End a session now' do
  field :address, :text
  replies_with 'c2h.destroyed', 'c2h.bounced'
end

# ---------------------------------------------------------------- c2h

message 'c2h.spawned', code: 0x0201, summary: 'A session exists' do
  doc 'For a recorded session this is the first record of its log.'
  field :address, :text
  field :revision, :bytes
  field :parent, :session, 'the invoking session, or 0'
  field :cause_seq, :seq, 'the h2c.spawn, or 0 for an <invoke>'
end

message 'c2h.input', code: 0x0202, summary: 'A recorded session took an input' do
  doc 'Written when a recorded session takes an envelope, before any ' \
      'effect of the macrostep it starts. It is the replay log: these ' \
      'records, fed back as h2c.event and h2c.invoke_report under ' \
      'h2c.spawn(replaying), rebuild the session.'
  doc 'Not written for an input a replaying session takes: that input is ' \
      'already in the log being replayed. A session restored from c2h.state ' \
      'and replayed starts writing c2h.input again after h2c.publish.'
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
end

message 'c2h.send', code: 0x0203, summary: 'A send for an I/O processor' do
  doc 'Mail to a local session never appears here: it is an envelope ' \
      '(reference/envelope.md). A delayed send to an I/O processor waits ' \
      'in the pool and is written when due. The host acts on it after the ' \
      "macrostep's c2h.fence."
  doc 'A delayed send that becomes due on h2c.time is written outside any ' \
      'macrostep: its `macrostep` is 0, no fence follows, and the host may ' \
      'act on it at once. Why: the macrostep that scheduled it committed at ' \
      'its own fence, and firing it changes no session, so there is nothing ' \
      'to commit and nothing to roll back (reference/envelope.md, "A due ' \
      'send to an I/O processor has no fence").'
  field :macrostep, :macrostep, '0 for a delayed send that became due'
  field :sendid, :text
  field :io_type, :text
  field :target, :text
  field :name, :text
  field :data, :vref, 'the namelist and params, aggregated by the data model'
  field :content, :vref, '<content>, or 0'
  see 'c2h.fence'
end

message 'c2h.invoke', code: 0x0204, summary: 'Start an invocation' do
  field :macrostep, :macrostep
  field :invokeid, :text
  field :type, :text
  field :src, :text
  field :data, :vref
  field :autoforward, :bool
  replies_with 'h2c.invoke_report'
end

message 'c2h.invoke_cancel', code: 0x0205, summary: 'Cancel an invocation' do
  field :macrostep, :macrostep
  field :invokeid, :text
end

message 'c2h.invoke_forward', code: 0x0206, summary: 'Autoforward an event to an invocation' do
  field :macrostep, :macrostep
  field :invokeid, :text
  field :name, :text
  field :sendid, :text
  field :origin, :text
  field :data, :vref
  field :type, :event_type
end

message 'c2h.log', code: 0x0207, summary: 'A <log>' do
  field :macrostep, :macrostep
  field :label, :text
  field :value, :vref
end

message 'c2h.fence', code: 0x0208, summary: 'A macrostep committed' do
  doc 'Everything the macrostep wrote to c2h comes before its fence. The ' \
      'host acts on none of it before the fence: a recorded session’s ' \
      'c2h.input is persisted first, then its effects are released ' \
      '(reference/macrostep.md, "The fence").'
  field :macrostep, :macrostep
  field :effects, :u32, 'records this macrostep wrote to c2h, the fence excluded'
  field :mail, :u32, 'envelopes it put in the pool'
  field :status, :fence_status
end

message 'c2h.wake', code: 0x0209, summary: 'Call core back at this time' do
  doc 'The earliest due delayed send in the pool changed. One timer in the ' \
      "host's event loop is enough."
  flag :none, 0, 'nothing is due: cancel the timer'
  field :due, :time
  replies_with 'h2c.time'
end

message 'c2h.bounced', code: 0x020A, summary: 'An h2c record was not run' do
  doc 'Core hands it back with a reason and keeps nothing of it. The ' \
      'host decides whether to post it again.'
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
  flag :has_wake, 0, 'wake_at is meaningful'
  field :address, :text
  field :image, :blob
  field :wake_at, :time, 'its earliest delayed send'
end

message 'c2h.state', code: 0x020E, summary: "A recorded session's state at a point of its log" do
  doc 'A checkpoint (core ADR 0091). Its delayed sends are inside the ' \
      'image, so it is one artifact. Replaying from it needs only the ' \
      'c2h.input records after input_seq.'
  field :address, :text
  field :image, :blob
  field :input_seq, :seq, 'the last c2h.input it includes'
end

message 'c2h.published', code: 0x020F, summary: 'A replayed session is live' do
  field :address, :text
  field :replayed, :u32, 'inputs it replayed'
  field :dropped_timers, :u32, 'held delayed sends due by `at`'
  field :retired_invokes, :u32
end

message 'c2h.finished', code: 0x0210, summary: 'A session reached a top-level final state' do
  doc "Mail to a local parent (done.invoke) is an envelope; this record is for the host's books."
  field :macrostep, :macrostep
  field :donedata, :vref
end

message 'c2h.destroyed', code: 0x0211, summary: 'A session is gone' do
  doc 'Envelopes still waiting for it are recycled as for h2c.load_failed.'
  field :address, :text
  field :reason, :destroy_reason
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
