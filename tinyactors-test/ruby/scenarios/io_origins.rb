# frozen_string_literal: true

# Core owns the I/O processor registry, and every origin can be answered
# (ADR 0068). Three instruments stand for the three storage classes of an
# origin:
# - io.peer makes COMPUTED origins, "peer://node/<token>";
# - io.connection makes SCOPED origins, one scope per open connection;
# - io.request makes MESSAGE origins, one per request, answered once.
#
# A chart answers whoever sent it an event with the standard idiom,
# <send typeexpr="_event.origintype" targetexpr="_event.origin">, and the
# instrument's journal shows where the reply went. The run's counting
# allocator fails teardown if any origin entry is left behind.
module IOOrigins
  PEER = 'urn:tinyactors:test:peer'
  CONNECTION = 'urn:tinyactors:test:connection'
  BASIC_HTTP = 'http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor'
  SERVICE = 'urn:tinyactors:test:origin-service'
  SCXML = 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor'
  LONG_URI = 'http://client.example/callbacks/requests/0123456789abcdef0123456789abcdef'

  REPLY = { to_expr: '$_event.origin', type_expr: '$_event.origintype' }.freeze

  def self.replier
    TinyactorsTest::Chart.define(:io_replier) do
      data origin: nil, origintype: nil, outcome: nil, processors: nil, steps: 0
      state :ready do
        transition on: 'ping', type: :internal do
          assign '.origin', '$_event.origin'
          assign '.origintype', '$_event.origintype'
          send_event 'pong', **IOOrigins::REPLY
        end
        transition on: 'twice', type: :internal do
          send_event 'pong', **IOOrigins::REPLY
          send_event 'pong', **IOOrigins::REPLY
        end
        # Several actions before the reply, so an action budget pauses it.
        transition on: 'slow', type: :internal do
          assign '.steps', '.steps + 1'
          assign '.steps', '.steps + 1'
          assign '.steps', '.steps + 1'
          send_event 'pong', **IOOrigins::REPLY
        end
        transition on: 'probe', type: :internal do
          assign '.processors', '$_ioprocessors'
        end
        transition on: 'error.communication', type: :internal do
          assign '.outcome', '"error.communication"'
        end
        transition on: 'error.execution', type: :internal do
          assign '.outcome', '"error.execution"'
        end
        transition on: 'stop', target: :stopped
      end
      final :stopped
    end
  end

  def self.ecma_prober
    TinyactorsTest::Chart.define(:io_prober_js, datamodel: 'ecmascript') do
      data processors: { expr: 'null' }
      state :ready do
        transition on: 'probe', type: :internal do
          assign 'processors', '_ioprocessors'
        end
      end
    end
  end

  # Invokes a non-SCXML service and answers it (SCXML 6.4).
  def self.service_client
    TinyactorsTest::Chart.define(:io_service_client) do
      data origin: nil, origintype: nil
      state :using do
        invoke type: IOOrigins::SERVICE, id: 'svc'
        transition on: 'svc.hello', type: :internal do
          assign '.origin', '$_event.origin'
          assign '.origintype', '$_event.origintype'
          send_event 'reply', **IOOrigins::REPLY
          send_event 'direct', to: '#_svc', type: IOOrigins::SERVICE
        end
        transition on: 'done.invoke.svc', target: :after do
          send_event 'bye', **IOOrigins::REPLY
        end
      end
      state :after
    end
  end

  # [event, target, refused] of the sends an instrument journaled.
  def self.sends(io)
    io.entries('send').map { |entry| [entry[:event_text], entry[:target], entry[:refused]] }
  end
end

TinyactorsTest.scenarios 'io origins' do
  # Catches: _ioprocessors that lists only the SCXML processor (SCXML 5.10
  # MUST list every processor), and a Basic HTTP entry without the location
  # C.2.3 requires.
  scenario '_ioprocessors lists every processor with its location' do
    endpoint_io(IOOrigins::PEER, 'io.peer')
    endpoint_io(IOOrigins::CONNECTION, 'io.connection')
    endpoint_io(IOOrigins::BASIC_HTTP, 'io.request')
    [IOOrigins.replier, IOOrigins.ecma_prober].each do |chart|
      actor = spawn(chart)
      actor.deliver 'probe'
      session = actor.inspect_actor.fetch(:session_id)
      processors = actor.data(chart.datamodel == 'ecmascript' ? '.processors' : '.processors')
      locations = processors.to_h { |type, entry| [type.to_s, entry[:location]] }
      assert_equal({ 'scxml' => "#_scxml_#{session}",
                     IOOrigins::SCXML => "#_scxml_#{session}",
                     IOOrigins::PEER => "peer://here/session/#{session}",
                     IOOrigins::CONNECTION => "ws://here/session/#{session}",
                     IOOrigins::BASIC_HTTP => "http://here/session/#{session}" }, locations,
                   "#{chart.datamodel}: every processor, each with a location")
    end
  end

  # Catches: a COMPUTED origin that is not rendered, or rendered with
  # another processor's type.
  scenario 'a reply to a computed origin reaches its peer' do
    peer = endpoint_io(IOOrigins::PEER, 'io.peer')
    actor = spawn(IOOrigins.replier)
    assert_equal 'ok', receive(peer, actor, 'ping', token: 7).fetch(:result)
    assert_equal({ origin: 'peer://node/7', origintype: IOOrigins::PEER },
                 actor.data.slice(:origin, :origintype))
    assert_equal [['pong', 'peer://node/7', nil]], IOOrigins.sends(peer)
  end

  # Catches: a SCOPED origin that renders another scope's URI after its own
  # closed, and a late reply that is lost silently instead of failing.
  scenario 'a reply to an open connection arrives, and a late one fails' do
    connection = endpoint_io(IOOrigins::CONNECTION, 'io.connection')
    actor = spawn(IOOrigins.replier)
    connection.control('open', processor: connection.processor, uri: 'ws://client/1')
    receive(connection, actor, 'ping', uri: 'ws://client/1')
    assert_equal [['pong', 'ws://client/1', nil]], IOOrigins.sends(connection)

    # The event waits while the actor is busy; its connection closes, and a
    # new one reuses the scope's record under another generation.
    connection.control('open', processor: connection.processor, uri: 'ws://client/2')
    assert_equal 'ok', actor.post('busy')
    assert_equal 'held', receive(connection, actor, 'ping', uri: 'ws://client/2', run: false, hold: true)
      .fetch(:result)
    connection.control('close', uri: 'ws://client/2')
    connection.control('open', processor: connection.processor, uri: 'ws://client/3')
    run_system('the busy event')
    assert_equal ['ok'], connection.control('flush').fetch(:results)
    run_system('the late ping')
    assert_equal nil, actor.data('.origin'), 'a closed scope renders no origin, not the next connection'
    assert_equal IOOrigins::CONNECTION, actor.data('.origintype'), 'the processor is still named'
    assert_equal 'error.communication', actor.data('.outcome')
    assert_equal ['pong', nil, 'a reply needs a target'], IOOrigins.sends(connection).last
  end

  # Catches: a MESSAGE origin that is not stored, a long URI that is cut at
  # the inline buffer, and a request answered twice.
  scenario 'a request is answered once' do
    request = endpoint_io(IOOrigins::BASIC_HTTP, 'io.request')
    actor = spawn(IOOrigins.replier)
    receive(request, actor, 'ping', uri: 'http://client/r/1')
    receive(request, actor, 'ping', uri: IOOrigins::LONG_URI)
    receive(request, actor, 'twice', uri: 'http://client/r/2')
    assert_equal [['pong', 'http://client/r/1', nil], ['pong', IOOrigins::LONG_URI, nil],
                  ['pong', 'http://client/r/2', nil], ['pong', 'http://client/r/2', 'the request was already answered']],
                 IOOrigins.sends(request)
    assert_equal 'error.communication', actor.data('.outcome')
  end

  # Catches: an origin reference lost or freed early while the event is
  # queued in a host, held through a paused macrostep, or refused by a
  # finished actor. Teardown fails on any entry left behind.
  scenario 'an origin lives as long as its event, wherever the event waits' do
    request = endpoint_io(IOOrigins::BASIC_HTTP, 'io.request')
    actor = spawn(IOOrigins.replier)

    # Queued in the host while the actor is busy.
    assert_equal 'ok', actor.post('busy')
    assert_equal 'held', receive(request, actor, 'ping', uri: IOOrigins::LONG_URI, run: false, hold: true)
      .fetch(:result)
    run_system('the busy event')
    assert_equal ['ok'], request.control('flush').fetch(:results)
    run_system('the queued ping')
    assert_equal ['pong', IOOrigins::LONG_URI, nil], IOOrigins.sends(request).last

    # Paused inside the macrostep by an action budget.
    receive(request, actor, 'slow', uri: 'http://client/r/slow', run: false)
    status = actor.resume(budget: 100, action_budget: 1)
    assert_equal 'paused', status
    status = actor.resume(budget: 100, action_budget: 1) while status == 'paused'
    assert_equal ['pong', 'http://client/r/slow', nil], IOOrigins.sends(request).last

    # Many requests: live allocations do not grow.
    receive(request, actor, 'ping', uri: 'http://client/r/warm')
    live = allocator.stats.fetch(:live_allocations)
    20.times { |index| receive(request, actor, 'ping', uri: "http://client/r/#{index}") }
    assert_equal live, allocator.stats.fetch(:live_allocations), 'per-message origins are reused'

    # Refused by a finished actor: the host releases it.
    actor.deliver 'stop'
    assert_equal 'err_done', receive(request, actor, 'ping', uri: IOOrigins::LONG_URI).fetch(:result)

    # A held event the host drops is released too.
    other = spawn(IOOrigins.replier)
    assert_equal 'ok', other.post('busy')
    receive(request, other, 'ping', uri: IOOrigins::LONG_URI, run: false, hold: true)
    assert_equal 1, request.control('drop').fetch(:dropped)
  end

  # Catches: an event from a non-SCXML invoked service that names the SCXML
  # processor as its origintype, so the reply idiom cannot reach the
  # service, before or after the invocation ended (SCXML 6.4, 5.10.1).
  scenario 'a reply to an invoked service goes through the service type' do
    service = scripted_invoker(IOOrigins::SERVICE)
    io = recording_io(IOOrigins::SERVICE)
    client = spawn(IOOrigins.service_client)
    start = service.started.first
    report_invoke(start, :event, event: 'svc.hello')
    assert_equal({ origin: '#_svc', origintype: IOOrigins::SERVICE }, client.data.slice(:origin, :origintype))
    report_invoke(start, :done)
    assert_equal [%w[reply #_svc], %w[direct #_svc], %w[bye #_svc]],
                 io.entries('send').map { |entry| [entry[:event_text], entry[:target]] }
    assert_equal [IOOrigins::SERVICE], io.entries('send').map { |entry| entry[:io_type] }.uniq
  end

  # Catches: an image that drops I/O origins, or keeps a pointer into the
  # system it was captured from. Every class comes back as a stored URI.
  scenario 'an origin of each class survives capture and reconstruction' do
    peer = endpoint_io(IOOrigins::PEER, 'io.peer')
    connection = endpoint_io(IOOrigins::CONNECTION, 'io.connection')
    request = endpoint_io(IOOrigins::BASIC_HTTP, 'io.request')
    connection.control('open', processor: connection.processor, uri: 'ws://client/image')
    [[peer, { token: 42 }, 'peer://node/42'],
     [connection, { uri: 'ws://client/image' }, 'ws://client/image'],
     [request, { uri: IOOrigins::LONG_URI }, IOOrigins::LONG_URI]].each do |io, args, uri|
      actor = spawn(IOOrigins.replier)
      assert_equal 'ok', receive(io, actor, 'ping', run: false, **args).fetch(:result)
      image = capture(actor)
      actor.destroy
      restored = core('ta_actor_reconstruct', ta.actor.reconstruct(system: system_handle, image: image)).fetch(:out)
      run_system("reconstructed #{io.kind}")
      copy = TinyactorsTest::Actor.new(self, restored, :io_replier, actor.definition)
      assert_equal uri, copy.data('.origin'), "#{io.kind}: the URI travels in the image"
      assert_equal ['pong', uri, nil], IOOrigins.sends(io).last, "#{io.kind}: the reply reaches it"
      core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: restored))
      destroy_image(image)
    end
    connection.control('close', uri: 'ws://client/image')
  end
end
