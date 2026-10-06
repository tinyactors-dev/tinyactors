# frozen_string_literal: true

# A plugin reports failure; the interpreter decides what that means for the
# operation being evaluated (invariant 8, ADR 0008). The plugin's fault is
# never the chart's event: a data model that cannot evaluate an expression
# and an I/O processor that cannot dispatch a send are both "the adapter
# said no", and they reach the chart as different events.
#
# Every row below follows a mandatory W3C assertion this suite already
# runs, so the table is the specification's and not this harness's:
#
# - error.execution: test277 (illegal data value), test286 (assign to an
#   invalid location), test298 and test343 (param location or expr),
#   test311 (location expression), test199 (unsupported send type).
# - error.communication: test521 ("if the Processor cannot dispatch the
#   event, it MUST place the error error.communication on the internal
#   event queue of the session that attempted to send"), test496 (a session
#   that does not exist or is inaccessible).
#
# An invoker that fails to start is SCXML 6.4 ("if the invocation fails ...
# the Processor MUST place error.execution"), which the imported suite does
# not cover, so it is pinned here.
module FaultContexts
  IO_TYPE = 'urn:tinyactors:test:faults'
  SERVICE = 'urn:tinyactors:test:faulty'

  # Data-model failures: the adapter raises while core evaluates something.
  # Each is [label, executable content, expected event].
  DATA_MODEL = [
    ['a condition that errors', :condition, 'error.execution'],
    ['an expression that errors', proc { assign '.n', 'error("boom")' }, 'error.execution'],
    ['an assign to a location that errors', proc { assign '.missing | error("boom")', '1' }, 'error.execution'],
    ['a script that errors', proc { script 'error("boom")' }, 'error.execution'],
    ['a log expression that errors', proc { log 'x', expr: 'error("boom")' }, 'error.execution'],
    ['raise data that errors', proc { raise_event 'other', data: 'error("boom")' }, 'error.execution'],
    ['send data that errors', proc { send_event 'other', data: 'error("boom")' }, 'error.execution'],
    ['a send delay that errors', proc { send_event 'other', delay_expr: 'error("boom")' }, 'error.execution'],
    ['a send event name that errors', proc { send_event event_expr: 'error("boom")' }, 'error.execution'],
    ['a send ID location that errors', proc { send_event 'other', id_location: '.x | error("boom")' },
     'error.execution'],
    ['a cancel ID that errors', proc { cancel expr: 'error("boom")' }, 'error.execution'],
    ['a param expression that errors', proc { send_event 'other', params: { p: 'error("boom")' } },
     'error.execution'],
    ['an unsupported send type', proc { send_event 'other', to: '#_internal', type: 'urn:no:such:processor' },
     'error.execution']
  ].freeze

  # A chart that runs `body` on entry and records the first error it sees.
  def self.chart(body)
    TinyactorsTest::Chart.define(:faulting) do
      data outcome: nil
      data n: 0
      state :ready do
        on_entry(&body)
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end
  end

  # The condition case needs a transition rather than executable content.
  def self.condition_chart
    TinyactorsTest::Chart.define(:faulting_condition) do
      data outcome: nil
      state :ready do
        transition on: 'go', cond: 'error("boom")', target: :other
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
      state :other
    end
  end

  def self.outcome(context, entry)
    _, body, _ = entry
    context.instance_exec do
      if body == :condition
        actor = spawn(FaultContexts.condition_chart)
        actor.deliver 'go'
        actor.data('.outcome')
      else
        spawn(FaultContexts.chart(body)).data('.outcome')
      end
    end
  end
end

TinyactorsTest.scenarios 'fault contexts' do
  # Catches: an adapter fault reported as whatever the adapter chose, or
  # one error class used for every failure. A data model that cannot
  # evaluate is an execution error, whatever it was asked to evaluate.
  scenario 'every data-model failure reaches the chart as error.execution' do
    expected = FaultContexts::DATA_MODEL.to_h { |label, _, event| [label, event] }
    actual = FaultContexts::DATA_MODEL.to_h do |entry|
      [entry.first, FaultContexts.outcome(self, entry)]
    end
    assert_equal expected, actual
  end

  # Catches: a failed dispatch reported as an execution error, which would
  # send a chart down its "I wrote something wrong" path instead of its "the
  # network is down" path (test521).
  scenario 'a failed dispatch reaches the sending chart as error.communication' do
    io = recording_io(FaultContexts::IO_TYPE)
    io.arm_fault(:send, message: 'the processor is down')
    sender = TinyactorsTest::Chart.define(:io_sender) do
      data outcome: nil
      state :ready do
        on_entry { send_event 'hit', to: 'somewhere', type: FaultContexts::IO_TYPE }
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end

    assert_equal 'error.communication', spawn(sender).data('.outcome')
    assert_equal [true], io.entries('send').map { |entry| entry[:injected] },
                 'the processor was asked once and refused'
  end

  # Catches: the same confusion one layer out. A router that refuses a
  # delivery is the host saying "undeliverable", which is test521's rule
  # again, not an authoring mistake. The ping goes to an address no
  # resident actor holds: mail to a resident one never reaches the router
  # (core ADR 0099).
  scenario 'a router that refuses a delivery is a communication error' do
    router = recording_router
    router.arm_fault(:submit, message: 'no route', count: 8)
    pinger = TinyactorsTest::Chart.define(:pinger) do
      data outcome: nil
      state :ready do
        on_entry { send_event 'ping', to: '#_actor_nowhere/n' }
        transition on: 'ping', type: :internal do
          assign '.outcome', '"ping"'
        end
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end

    assert_equal 'error.communication', spawn(pinger).data('.outcome')
  end

  # Catches: an invoker failure reported as a communication error. SCXML
  # 6.4 makes starting an invocation part of executing the chart, so a
  # service that will not start is an execution error - unlike a service
  # that started and was later lost, which is
  # error.communication.invoke.<id>.unavailable (ADR 0039).
  scenario 'an invoker that will not start is an execution error' do
    service = scripted_invoker(FaultContexts::SERVICE)
    service.arm_fault(:start, message: 'no capacity')
    client = TinyactorsTest::Chart.define(:faulty_client) do
      data outcome: nil
      state :calling do
        invoke type: FaultContexts::SERVICE, id: :svc
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end

    caller = spawn(client)
    assert_equal 'error.execution', caller.data('.outcome')
    assert_equal [true], service.started.map { |entry| entry[:injected] }
    assert_equal [], caller.observe.fetch(:invokes), 'no invocation is left behind'
  end

  # Catches: an unknown invoker type treated as a communication failure, or
  # silently ignored. SCXML 6.4 requires an error for a type the processor
  # does not support.
  scenario 'an unknown invoker type is an execution error' do
    client = TinyactorsTest::Chart.define(:unknown_client) do
      data outcome: nil
      state :calling do
        invoke type: 'urn:tinyactors:test:nosuch', id: :svc
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end

    assert_equal 'error.execution', spawn(client).data('.outcome')
  end
end
