# These fixtures use only the public Raw API and execute the real C runtime.
module RubyRuntimeProbes
  def ok(result)
    assert(result.is_a?(raw::Result), 'Expected Raw::Result')
    equal(constant('TA_OK'), result.status)
    result.value
  end
  def arena_options
    {initial_chunk_bytes: 4096, maximum_chunk_bytes: 65536,
     maximum_allocation: 65536, maximum_bytes: constant('TA_UNLIMITED')}
  end
  def with_system(data = {})
    arena = ok(raw.ta_arena_create(raw::Allocator.malloc, arena_options))
    system = nil
    begin
      config = raw.ta_system_config_init.merge(data_model: 'bytes', session_prefix: 'verify', data: data, host_user: self)
      system = ok(raw.ta_system_create(arena, config))
      yield system
    ensure
      if system
        # Individual probes own their actors/definitions and release them first.
        equal(constant('TA_OK'), raw.ta_system_destroy(system))
        equal(true, system.closed?)
      end
      raw.ta_arena_close(arena)
      equal(true, arena.closed?)
    end
  end
  def run_raw(system, time = 0, work = 1000)
    result = ok(raw.ta_system_run(system, {now_ns: time, budget: {work_units: work, actor_quantum: 100}}))
    %i[work_units microsteps external_events terminal_actors runnable io_ready next_deadline_ns].each { |key| assert(result.key?(key), "Missing run field #{key}") }
    assert([true, false].include?(result[:runnable]), 'runnable is a semantic boolean')
    result
  end
  def action(code)
    {kind: constant('TA_ACTION_CUSTOM'), as: {custom: {code: code}}}
  end
  def transition(event, code, **fields)
    {event: event, content: {actions: [action(code)]}, **fields}
  end
  def chart(states)
    {name: 'verification', data_model: 'bytes', states: states,
     data_layout: {storage: constant('TA_DATA_BYTES'), bytes_layout: {size: 8, alignment: 8}}}
  end
  def build_raw(system, states)
    ok(raw.ta_definition_build(system, chart(states))).fetch(:definition)
  end
  def spawn_raw(system, definition, **fields)
    ok(raw.ta_actor_spawn(system, {definition: definition, **fields}))
  end
  def send_raw(context, name, target, delay = 0)
    raw.ta_context_send(context, {event: name, target: target, delay_ns: delay,
      body: {kind: constant('TA_BODY_CONTENT'), content: {kind: constant('TA_PAYLOAD_BYTES'), as: {bytes: "\x00\xff".b}}}})
  end
  def post_raw(system, actor, name)
    ok(raw.ta_post(system, {target: {kind: constant('TA_TARGET_INSTANCE'), instance: actor},
                           message: {name: name, data: {kind: constant('TA_PAYLOAD_NONE')}}}))
  end
  def dispose(system, actors, definitions, time = 0)
    actors.each { |actor| equal(constant('TA_OK'), raw.ta_actor_stop(system, actor)) }
    run_raw(system, time)
    actors.each { |actor| equal(constant('TA_OK'), raw.ta_actor_release(system, actor)) }
    definitions.each { |definition| equal(nil, raw.ta_definition_release(definition)) }
  end
  def probe_reference(c)
    with_system do |system|
      type = klass(c.fetch('klass'))
      if c['invalid']
        rejects(RangeError) { type.new(system, c.fetch('id_value')) }
        next
      end
      ref = type.new(system, c.fetch('id_value'))
      equal(c.fetch('id_value'), ref.id)
      assert(ref.system.equal?(system))
      equal(ref, type.new(system, ref.id))
      equal(ref.hash, type.new(system, ref.id).hash)
      assert(ref != type.new(system, (ref.id + 1) % (2**64)))
      assert(ref.frozen?)
      with_system { |other| assert(ref != type.new(other, ref.id), 'Reference equality lost system identity') }
    end
  end
  def probe_arena(c)
    arena = ok(raw.ta_arena_create(raw::Allocator.malloc, arena_options))
    begin
      case c.fetch('case')
      when 'defer'
        seen = []
        objects = [Object.new, Object.new]
        objects.each { |object| equal(constant('TA_OK'), raw.ta_arena_defer(arena, ->(arg) { seen << arg; nil }, object)) }
        GC.start
        GC.compact if GC.respond_to?(:compact)
        raw.ta_arena_close(arena)
        equal(objects.reverse, seen)
      when 'siblings'
        children = 2.times.map { ok(raw.ta_arena_open(arena, arena_options)) }
        memory = ok(raw.ta_arena_alloc(children[1], {size: 8, alignment: 8}))
        memory.write(0, 'survives')
        raw.ta_arena_close(children[0])
        equal('survives', memory.read(0, 8))
        raw.ta_arena_close(arena)
        equal(true, children[1].closed?)
        rejects(RuntimeError) { memory.read(0, 1) }
      when 'limit'
        stats = raw.ta_arena_stats(arena)
        assert(stats.fetch(:reserved_bytes) > 0)
        assert(raw.ta_arena_set_limit(arena, 0) != constant('TA_OK'))
        assert(raw.ta_arena_alloc(arena, {size: 0, alignment: 1}).status != constant('TA_OK'))
        memory = ok(raw.ta_arena_alloc(arena, {size: 8, alignment: 8}))
        memory.write(0, 'still-ok')
        equal('still-ok', memory.read(0, 8))
      end
    ensure
      raw.ta_arena_close(arena) unless arena.closed?
    end
    equal(true, arena.closed?)
  end
  def probe_system(c)
    with_system do |system|
      case c.fetch('case')
      when 'clock'
        run_raw(system, 100)
        equal(constant('TA_INVALID'), raw.ta_system_run(system, {now_ns: 99, budget: {work_units: 1, actor_quantum: 1}}).status)
        run_raw(system, 100)
        equal(constant('TA_TIME_NEVER'), raw.ta_system_next_deadline(system))
      when 'budget'
        equal(0, run_raw(system, 0, 0).fetch(:work_units))
      when 'unknown_field'
        rejects(ArgumentError) { raw.ta_system_run(system, {now_ns: 0, budget: {work_units: 1, actor_quantum: 1}, typoo: true}) }
      end
    end
  end
  def probe_runtime(c)
    mode = c.fetch('case')
    events, escaped, actors, definitions = [], [], [], []
    sender = receiver = nil
    time = 100
    delayed = mode == 'delayed_reply'
    data = {
      test: lambda do |ctx, code|
        view = raw.ta_context_inspect(ctx)
        events << [:guard, view.fetch(:system).fetch(:event)]
        if mode == 'guard_effect'
          equal(constant('TA_INVALID'), send_raw(ctx, 'ILLEGAL', {kind: constant('TA_TARGET_SELF')}).status)
        end
        raw::Result.new(status: constant('TA_OK'), value: mode != 'false_guard')
      end,
      run: lambda do |ctx, kind, code|
        equal(constant('TA_ACTION_CUSTOM'), kind)
        equal(self, raw.ta_context_host_user(ctx))
        view = raw.ta_context_inspect(ctx)
        event = view.fetch(:system).fetch(:event)
        events << [code, event]
        escaped << ctx
        case code
        when 1
          # Host trigger has no origin. This explicit send creates actor origin.
          equal(nil, event.fetch(:origin))
          ok(send_raw(ctx, 'PING', {kind: constant('TA_TARGET_INSTANCE'), instance: receiver}))
        when 2
          if mode == 'exception'
            raise 'intentional callback failure'
          elsif mode == 'internal'
            equal(constant('TA_OK'), raw.ta_context_raise(ctx, 'INNER', nil))
          elsif mode == 'bytes'
            memory = ok(raw.ta_context_bytes(ctx))
            memory.write(0, "\x01\xff\x00abcde".b)
            escaped << memory
          else
            ok(send_raw(ctx, 'PONG', {kind: constant('TA_TARGET_URI'), text: event.fetch(:origin)}, delayed ? 50 : 0))
          end
        end
        constant('TA_OK')
      end
    }
    with_system(data) do |system|
      begin
        sender_def = build_raw(system, [{id: 'idle', transitions: [transition('GO', 1), transition('PONG', 3)]}])
        definitions << sender_def
        receiver_states = [{id: 'idle', onentry: {blocks: [{actions: [action(4)]}]},
          transitions: [transition('PING', 2, condition: 1), transition('INNER', 5), transition('error.execution', 6), transition('ILLEGAL', 7)]}]
        if mode == 'eventless'
          receiver_states[0][:transitions][0][:target] = 'middle'
          receiver_states << {id: 'middle', transitions: [transition(nil, 8, target: 'end')]}
          receiver_states << {id: 'end'}
        end
        receiver_def = build_raw(system, receiver_states)
        definitions << receiver_def
        sender = spawn_raw(system, sender_def, name: 'sender')
        actors << sender
        receiver = spawn_raw(system, receiver_def)
        actors << receiver
        assert(events.empty?, 'Spawn executed actions synchronously')
        run_raw(system, time)
        equal(nil, events.find { |tag, _| tag == 4 }.last)
        origin = '#_scxml_' + ok(raw.ta_actor_inspect(system, sender)).fetch(:sessionid)
        post_raw(system, sender, 'GO')
        run_raw(system, time)
        guard = events.find { |tag, _| tag == :guard }.last
        equal(origin, guard.fetch(:origin))
        equal(constant('TA_SCXML_IOPROCESSOR'), guard.fetch(:origintype))
        if mode == 'false_guard'
          assert(events.none? { |tag, _| tag == 2 || tag == 3 }, 'False guard executed its action or reply')
        else
          ping = events.find { |tag, _| tag == 2 }.last
          equal(origin, ping.fetch(:origin))
          equal("\x00\xff".b, ping.fetch(:data).fetch(:as).fetch(:bytes))
          case mode
          when 'internal'
            inner = events.find { |tag, _| tag == 5 }.last
            equal(constant('TA_EVENT_INTERNAL'), inner.fetch(:type))
            equal(nil, inner.fetch(:origin))
          when 'bytes'
            equal("\x01\xff\x00abcde".b, ok(raw.ta_actor_inspect(system, receiver)).fetch(:state).fetch(:as).fetch(:bytes))
          when 'exception'
            equal(1, events.count { |tag, _| tag == 6 })
            errors = raw.take_errors(system)
            equal(1, errors.count { |error| error.message == 'intentional callback failure' })
            equal([], raw.take_errors(system))
          else
            if delayed
              assert(events.none? { |tag, _| tag == 3 })
              equal(150, raw.ta_system_next_deadline(system))
              run_raw(system, 149)
              assert(events.none? { |tag, _| tag == 3 })
              time = 150
              run_raw(system, time)
            end
            equal(1, events.count { |tag, _| tag == 3 })
            pong = events.find { |tag, _| tag == 3 }.last
            equal('#_scxml_' + ok(raw.ta_actor_inspect(system, receiver)).fetch(:sessionid), pong.fetch(:origin))
            equal(origin, events.find { |tag, _| tag == 8 }.last.fetch(:origin)) if mode == 'eventless'
          end
        end
        assert(events.none? { |tag, _| tag == 7 }, 'Guard side effect delivered a message')
        escaped.each do |view|
          if view.is_a?(raw::Context)
            rejects(RuntimeError) { raw.ta_context_inspect(view) }
            rejects(RuntimeError) { raw.ta_context_host_user(view) }
            rejects(RuntimeError) { raw.ta_context_in(view, 'idle') }
            rejects(RuntimeError) { raw.ta_context_cancel(view, 'unused') }
          else
            rejects(RuntimeError) { view.read(0, 1) }
          end
        end
        GC.start
        GC.compact if GC.respond_to?(:compact)
        equal(origin, guard.fetch(:origin))
        if mode == 'busy'
          equal(constant('TA_BUSY'), raw.ta_system_destroy(system))
          equal(false, system.closed?)
          ok(raw.ta_actor_inspect(system, sender))
        end
        if mode == 'stale'
          equal(constant('TA_OK'), raw.ta_actor_stop(system, sender))
          run_raw(system, time)
          equal(constant('TA_OK'), raw.ta_actor_release(system, sender))
          actors.delete(sender)
          equal(constant('TA_STALE'), raw.ta_actor_inspect(system, sender).status)
        end
      ensure
        dispose(system, actors, definitions, time)
      end
    end
  end
  def probe_high_reply(c)
    verifier = self
    mode = c.fetch('case')
    seen, origins, saved, errors = [], [], [], []
    system = TinyActors::System.new(config: {data_model: 'bytes', session_prefix: 'highverify'})
    actors, definitions = [], []
    receiver = nil
    time = 100
    begin
      layout = {storage: constant('TA_DATA_BYTES'), bytes_layout: {size: 0, alignment: 1}}
      a_spec = TinyActors.machine('sender', data_model: 'bytes', data_layout: layout) do
        state 'idle' do
          on 'GO', actions: [->(ctx) { ctx.send('PING', target: receiver.to_target, payload: :none) }]
          on 'PONG', actions: [->(ctx) { seen << ctx.event.name; saved << ctx }]
        end
      end
      b_spec = TinyActors.machine('receiver', data_model: 'bytes', data_layout: layout) do
        state 'idle' do
          on 'PING', guard: ->(ctx) { origins << ctx._event.origin; true }, actions: [->(ctx) {
            verifier.assert(ctx.event.equal?(ctx._event))
            origins << ctx.event.origin
            saved << ctx
            begin
              ctx.reply('PONG', payload: :none, delay_ns: mode == 'delayed' ? 50 : 0)
            rescue TinyActors::Error => error
              errors << error
              raise
            end
          }]
        end
      end
      definitions << system.define(a_spec)
      definitions << system.define(b_spec)
      actors << system.spawn(definitions[0])
      receiver = system.spawn(definitions[1])
      actors << receiver
      system.run(now_ns: time)
      system.post(mode == 'missing_origin' ? receiver.to_target : actors[0].to_target, mode == 'missing_origin' ? 'PING' : 'GO', payload: :none)
      system.run(now_ns: time)
      if mode == 'missing_origin'
        equal([], seen)
        equal(1, errors.size)
        equal(constant('TA_EXECUTION_ERROR'), errors[0].status)
        equal('Context#reply', errors[0].operation)
      else
        equal(2, origins.size)
        assert(origins[0].start_with?('#_scxml_'))
        equal(origins[0], origins[1])
        if mode == 'delayed'
          equal([], seen)
          equal(150, system.next_deadline)
          system.run(now_ns: 149)
          equal([], seen)
          time = 150
          system.run(now_ns: time)
        end
        equal(['PONG'], seen)
      end
      saved.each do |ctx|
        rejects(RuntimeError) { ctx.event }
        rejects(RuntimeError) { ctx.reply('LATE', payload: :none) }
      end
    ensure
      actors.each(&:stop)
      system.run(now_ns: time)
      actors.each(&:release)
      definitions.each(&:release)
      system.close
    end
  end
end
