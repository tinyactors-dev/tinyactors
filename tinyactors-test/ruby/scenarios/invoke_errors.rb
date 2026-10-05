# frozen_string_literal: true

# A host reports an invocation error with its own reason (core ADR 0081):
# error.communication.invoke.<id>.<reason> on the invoking session's
# internal queue. With `ends` false the invocation stays active, so its
# events, done.invoke.<id> and cancellation behave as if no report was
# made; with `ends` true it is retired. napi uses `ends` false to tell a
# parent that its child was quarantined (napi ADR 0025).

module InvokeErrors
  SERVICE = 'urn:tinyactors:test:service'
end

# Waits in :calling for the service; logs every invocation error it sees
# and every event from the service, and leaves on `leave` or done.invoke.
TinyactorsTest.chart :error_client do
  data errors: { expr: '[]' }
  data replies: { expr: '[]' }
  data error_data: nil

  state :calling do
    invoke type: InvokeErrors::SERVICE, id: :svc
    transition on: 'error.communication.invoke.svc', type: :internal do
      assign '.errors', '.errors + [$_event.name]'
      assign '.error_data', '$_event.data'
    end
    transition on: 'reply', type: :internal do
      assign '.replies', '.replies + [$_event.data]'
    end
    transition on: 'done.invoke.svc', target: :finished
    transition on: 'leave', target: :left
    # A macrostep of several microsteps, to pause inside.
    transition on: 'work', type: :internal do
      raise_event 'step.one'
    end
    transition on: 'step.one', type: :internal do
      raise_event 'step.two'
    end
    transition on: 'step.two', type: :internal do
      assign '.replies', '.replies + ["worked"]'
    end
  end

  state :finished
  state :left
end

TinyactorsTest.scenarios 'invocation errors' do
  # Catches: an error report that retires the invocation or ends the wait,
  # so the parent could no longer get done.invoke from a child that
  # recovered.
  scenario 'an error that does not end the invocation leaves it active' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    report_invoke(start, :error, reason: 'quarantined', data: { code: 'runaway', message: 'too many' })
    assert_active client, :calling
    assert_equal ['error.communication.invoke.svc.quarantined'], client.data('.errors')
    assert_equal({ code: 'runaway', message: 'too many' }, client.data('.error_data'))
    assert_equal 1, client.observe[:invokes].size, 'the invocation is still active'

    report_invoke(start, :event, event: 'reply', data: 7)
    assert_equal [7], client.data('.replies'), 'events from the service still arrive'
    report_invoke(start, :done)
    assert_active client, :finished
  end

  # Catches: an error report that disturbs cancellation.
  scenario 'leaving the invoking state still cancels an invocation that reported an error' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    report_invoke(start, :error, reason: 'quarantined')
    client.deliver 'leave'
    assert_active client, :left
    assert_equal [start[:service]], service.entries('cancel').map { |entry| entry[:service] }
  end

  # Catches: `ends` true that differs from the old UNAVAILABLE: the
  # invocation must be retired and its later events ignored.
  scenario 'an error that ends the invocation retires it' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    report_invoke(start, :error, reason: 'unavailable', ends: true)
    assert_equal ['error.communication.invoke.svc.unavailable'], client.data('.errors')
    assert_equal [], client.observe[:invokes], 'no invocation is active'
    late = ta.actor.report_invoke(system: system_handle, actor: start[:parent], invoke_id: start[:invoke_id],
                                  kind: 'done')
    assert_equal 'err_not_found', late[:result]
  end

  # Catches: a report taken inside a macrostep, where a log could not say
  # where it fell (core ADR 0089 section 4): inside one, core admits it and
  # holds it back, and it changes nothing until the macrostep ends; then it
  # is taken as an input of its own.
  scenario 'a report into a paused macrostep is held and taken once the macrostep ends' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    assert_equal 'ok', client.post('work')
    assert_equal 'paused', client.resume(budget: 1)
    error = { reason: 'quarantined', ends: false }
    report = ta.actor.report_invoke(system: system_handle, actor: start[:parent], invoke_id: start[:invoke_id],
                                    kind: 'error', error: error)
    assert_equal 'ok', report[:result]
    status = 'paused'
    20.times do
      status = client.resume(budget: 1)
      break unless status == 'paused'
    end
    assert_equal 'stable', status
    assert_equal ['worked'], client.data('.replies')
    assert_equal [], client.data('.errors'), 'the held report did not enter the macrostep'
    assert_equal 'stable', client.resume(budget: 100), 'the held report is the next input'
    assert_equal ['error.communication.invoke.svc.quarantined'], client.data('.errors')
  end

  # Catches: a reason that is not one event-name token, which would make
  # an event name the chart cannot match as intended.
  scenario 'a reason that is not one token is refused and changes nothing' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    ['', 'two.tokens', 'white space'].each do |reason|
      report = ta.actor.report_invoke(system: system_handle, actor: start[:parent], invoke_id: start[:invoke_id],
                                      kind: 'error', error: { reason: reason, ends: true })
      assert_equal 'err_invalid', report[:result], reason.inspect
    end
    run_system('after refused reports')
    assert_equal [], client.data('.errors')
    assert_equal 1, client.observe[:invokes].size, 'the invocation is still active'
  end
  # Catches: a report that, out of memory, retires the invocation or queues
  # half an event. Each allocation it makes is refused in turn; every
  # refusal changes nothing, and the report then goes through.
  scenario 'a report that runs out of memory changes nothing' do
    service = scripted_invoker(InvokeErrors::SERVICE)
    client = spawn :error_client
    start = service.started.first
    failures = 0
    result = nil
    32.times do |failing|
      allocator.arm_fault(after: failing, count: 1)
      report = ta.actor.report_invoke(system: system_handle, actor: start[:parent], invoke_id: start[:invoke_id],
                                      kind: 'error', error: { reason: 'crashed', ends: true, data: { n: failing } })
      allocator.clear_faults
      result = report[:result]
      break unless result == 'err_nomem'

      failures += 1
      assert_equal 1, client.observe[:invokes].size, "allocation #{failing + 1}: the invocation is still active"
      assert_equal [], client.observe[:internal_events], "allocation #{failing + 1}: nothing is queued"
    end
    assert_equal 'ok', result
    assert failures.positive?, 'the report allocates'
    run_system('after the report')
    assert_equal ['error.communication.invoke.svc.crashed'], client.data('.errors')
    assert_equal({ n: failures }, client.data('.error_data'))
    assert_equal [], client.observe[:invokes]
  end

  # Catches: a host that cannot tell whom an invoked child's fault concerns.
  # Inspection names the child's invocation, which a host reports to, and
  # the step the child is at (core ADR 0081).
  scenario 'inspection names an invoked child\'s parent and invocation' do
    parent = spawn :caller
    child = invoked_actors.fetch(0)
    view = child.inspect_actor
    assert_equal true, view[:has_parent]
    assert_equal parent.handle, view.dig(:parent, :actor)
    assert_equal parent.inspect_actor.fetch(:invokes).first.fetch(:id), view.dig(:parent, :invoke_id)
    assert_equal view[:session_id], view.dig(:trace, :cause, :session)
    assert_equal false, parent.inspect_actor[:has_parent], 'a spawned actor has no parent'

    report = ta.actor.report_invoke(system: system_handle, actor: view.dig(:parent, :actor),
                                    invoke_id: view.dig(:parent, :invoke_id), kind: 'error',
                                    error: { reason: 'quarantined', ends: false })
    assert_equal 'ok', report[:result]
  end
end
