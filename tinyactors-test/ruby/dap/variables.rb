# frozen_string_literal: true

module DAP
  # Scopes and variables. Every frame of a thread shows the same seven
  # scopes, because they belong to the actor, not to a frame:
  #
  # | Data model    | the data root (ta_actor_datamodel_root), read again on each expansion |
  # | Event         | _event of the step the actor is in                                     |
  # | Configuration | the active states as a tree, and what each history state recorded      |
  # | Queues        | the internal queue, the macrostep's event, debugger messages waiting   |
  # | Timers        | pending delayed sends and when each is due                             |
  # | Invocations   | active invocations, and the thread of each child                       |
  # | System        | _sessionid, _name, _ioprocessors, logical time, the definition         |
  #
  # A variablesReference is valid until the system runs again (`reset`).
  class Variables
    SCOPES = ['Data model', 'Event', 'Configuration', 'Queues', 'Timers', 'Invocations', 'System'].freeze
    PREVIEW = 60

    def initialize(session:, labels:, threads:, control:, expressions:, console:)
      @session = session
      @labels = labels
      @threads = threads
      @control = control
      @expressions = expressions
      @console = console
      reset
    end

    def context = @session.context

    def reset
      @refs = {}
      @next = 0
    end

    def scopes(thread)
      SCOPES.map do |name|
        ref = name == 'Data model' ? add([:data, thread.id, []]) : add([:scope, thread.id, name])
        { 'name' => name, 'variablesReference' => ref, 'expensive' => false,
          'presentationHint' => name == 'Data model' ? 'locals' : nil }.compact
      end
    end

    # The children of a reference, `start` and `count` paging them.
    def variables(reference, start: 0, count: 0, filter: nil)
      entry = @refs[Integer(reference)] or raise RequestError, "variablesReference #{reference} is not valid (any more)"

      children = children_of(entry)
      children = children.select { |child| filter == 'indexed' ? child[:index] : !child[:index] } if filter
      children = children.drop(start.to_i)
      children = children.first(count.to_i) if count.to_i.positive?
      children.map { |child| child[:variable] }
    end

    # A value as evaluate and setVariable answer it: text, and a reference
    # when it has children.
    def present(value, base: nil, data: nil)
      ref = container?(value) ? add(data || [:value, value, base]) : 0
      body = { 'result' => preview(value), 'type' => type_of(value), 'variablesReference' => ref }
      body['indexedVariables'] = value.size if value.is_a?(Array)
      body['namedVariables'] = value.size if value.is_a?(Hash)
      body
    end

    # setVariable: only the data model can be written.
    def set(reference, name, text)
      entry = @refs[Integer(reference)] or raise RequestError, "variablesReference #{reference} is not valid (any more)"
      raise RequestError, 'only the Data model can be written; the other scopes are read-only views of core state' unless entry[0] == :data

      thread = thread!(entry[1])
      parent = entry[2]
      root = @expressions.root(thread)
      container = dig(root, parent)
      key = container.is_a?(Array) ? Integer(name.delete('[]')) : name
      write(thread, parent + [key], text)
    end

    # setExpression: an expression that names a location.
    def assign(thread, expression, text)
      path = @expressions.path(thread, expression, event: event_of(thread))
      write(thread, path, text)
    end

    # The dataId of a variable, for dataBreakpointInfo.
    def data_id(reference, name)
      entry = @refs[Integer(reference)]
      return nil unless entry && entry[0] == :data

      thread = thread!(entry[1])
      container = dig(@expressions.root(thread), entry[2])
      key = container.is_a?(Array) ? Integer(name.delete('[]')) : name
      [thread, entry[2] + [key]]
    rescue ArgumentError
      nil
    end

    private

    def add(entry)
      @next += 1
      @refs[@next] = entry
      @next
    end

    def thread!(id)
      @threads[id] or raise RequestError, "thread #{id} is gone"
    end

    # Writes a data-model location of one actor.
    #
    # Core has no operation that changes an actor's data root, and the
    # jq data model's instance gives out copies. What the test host can do
    # with public API is the residency round trip of ADR 0038: capture an
    # image, unload the actor, and load it again with a changed root as its
    # data-model binding. The configuration, the pending delayed sends and
    # a macrostep paused between units (ADR 0061) all survive. The costs are
    # real, so they are refused rather than paid silently: only a named
    # actor can be unloaded, and a load turns every active invocation into
    # error.communication.
    def write(thread, path, text)
      raise RequestError, "#{thread.label} is an invoked child, which has no address to unload and load it at" unless thread.named?

      view = @session.inspect_actor(thread.handle)
      invokes = Array(view&.dig(:invokes)).count { |invoke| invoke[:active] }
      if invokes.positive?
        raise RequestError, "#{thread.label} has #{invokes} active invocation(s), which reloading it to write " \
                            'the data would end with error.communication'
      end

      value = @expressions.literal(thread, text, event: event_of(thread))
      root = @expressions.root(thread)
      root = context.rpc('json.transform', filter: "setpath(#{JSON.generate(path)}; #{JSON.generate(value)})",
                                           input: root).first
      reload(thread.actor, root)
      @threads.refresh
      present(value, data: [:data, thread.id, path]).then do |body|
        { 'value' => body['result'], 'type' => body['type'], 'variablesReference' => body['variablesReference'] }
      end
    end

    def reload(actor, root)
      image = context.capture(actor)
      unloaded = context.unload(actor, image)
      raise RequestError, "ta_actor_unload answered #{unloaded}" unless unloaded == 'ok'

      loaded = context.load(actor, image, binding: root)
      raise RequestError, "ta_actor_load answered #{loaded}; #{actor} is unloaded" unless loaded == 'ok'
    ensure
      context.destroy_image(image) if image
    end

    def event_of(thread) = @control.event_of(thread)

    # ------------------------------------------------------------------

    def children_of(entry)
      case entry[0]
      when :data
        thread = thread!(entry[1])
        path = entry[2]
        value = dig(@expressions.root(thread), path)
        entries(value).map do |key, child, index|
          child_path = path + [key]
          variable(key, child, index, evaluate: Expressions.location(child_path),
                                      data: [:data, entry[1], child_path])
        end
      when :value
        entries(entry[1]).map { |key, child, index| variable(key, child, index, base: entry[2]) }
      when :scope
        scope(thread!(entry[1]), entry[2]).map { |key, child| variable(key, child, nil) }
      end
    end

    def entries(value)
      case value
      when Hash then value.map { |key, child| [key.to_s, child, false] }
      when Array then value.each_with_index.map { |child, index| [index, child, true] }
      else []
      end
    end

    def variable(key, value, index, evaluate: nil, data: nil, base: nil)
      ref = container?(value) ? add(data || [:value, value, nil]) : 0
      name = index ? "[#{key}]" : key.to_s
      variable = { 'name' => name, 'value' => preview(value), 'type' => type_of(value), 'variablesReference' => ref }
      variable['evaluateName'] = evaluate if evaluate
      variable['indexedVariables'] = value.size if value.is_a?(Array)
      variable['namedVariables'] = value.size if value.is_a?(Hash)
      { variable: variable, index: index }
    end

    def dig(value, path)
      path.reduce(value) do |inner, key|
        case inner
        when Hash then inner.fetch(key.to_s) { inner.fetch(key.to_sym, nil) }
        when Array then key.is_a?(Integer) ? inner[key] : nil
        end
      end
    end

    def container?(value) = (value.is_a?(Hash) || value.is_a?(Array)) && !value.empty?

    def type_of(value)
      case value
      when Hash then 'object'
      when Array then 'array'
      when String then 'string'
      when Integer, Float then 'number'
      when true, false then 'boolean'
      when nil then 'null'
      end
    end

    def preview(value)
      text = JSON.generate(value)
      return text if text.length <= PREVIEW

      case value
      when Hash then "{…} #{value.size} keys"
      when Array then "[…] #{value.size} items"
      else "#{text[0, PREVIEW]}…"
      end
    end

    # ------------------------------------------------------------------
    # The read-only scopes, as [name, value] pairs.

    def scope(thread, name)
      view = @session.inspect_actor(thread.handle) or raise RequestError, "#{thread.label} is not resident"

      case name
      when 'Event' then (@expressions.scxml_event(event_of(thread)) || {}).map { |key, value| [key.to_s, value] }
      when 'Configuration' then configuration(thread, view)
      when 'Queues' then queues(thread, view)
      when 'Timers' then timers(view)
      when 'Invocations' then invocations(view)
      when 'System' then system(thread, view)
      else []
      end
    end

    def configuration(thread, view)
      definition = view[:definition_id]
      response = context.ta.actor.active_states(system: context.system_handle, actor: thread.handle,
                                                capacity: view.fetch(:active_state_count))
      active = Array(response.dig(:out, :states))
      tree = {}
      active.each do |state|
        chain = []
        at = state
        while at
          chain.unshift(@labels.state(definition, at))
          at = @labels.state_parent(definition, at)
        end
        chain.reduce(tree) { |node, key| node[key] ||= {} }
      end
      pairs = [['active', tree]]
      history = @labels.states(definition).select { |_, info| info[:kind].to_s.include?('history') }
      recorded = history.to_h do |id, info|
        [info[:id], history_of(thread, id).map { |state| @labels.state(definition, state) }]
      end
      pairs << ['history', recorded] unless recorded.empty?
      pairs
    end

    def history_of(thread, state)
      query = context.ta.actor.history(system: context.system_handle, actor: thread.handle, history: state, capacity: 0)
      count = query.dig(:out, :count).to_i
      return [] if count.zero?

      response = context.ta.actor.history(system: context.system_handle, actor: thread.handle, history: state,
                                          capacity: count)
      Array(response.dig(:out, :states))
    end

    def queues(thread, view)
      internal = Array(view[:internal_events]).map { |queued| @expressions.scxml_event(queued[:event]) }
      current = view[:current_event] ? @expressions.scxml_event(view[:current_event]) : nil
      waiting = @console.queued_for(thread).map { |message| { name: message[:event], data: message[:data] } }
      [['internal', internal], ['current', current], ['debugger', waiting]]
    end

    def timers(view)
      now = context.now
      Array(view[:delayed_sends]).map do |send|
        target = context.send_target(view[:definition_id], send)
        [send[:send_id].to_s, { event: context.event_text(send[:event]), target: target, due: send[:due],
                                in: "#{Integer(send[:due]) - now}ms", data: send[:data] }]
      end
    end

    def invocations(view)
      Array(view[:invokes]).map do |invoke|
        child = @threads.by_handle(Integer(invoke[:service_handle])) rescue nil
        [invoke[:id].to_s, { type: context.ta.system.atom_text(system: context.system_handle, atom: invoke[:type])
                                          .dig(:out, :out),
                             active: invoke[:active], autoforward: invoke[:autoforward],
                             thread: child ? "#{child.id} #{child.name}" : nil }]
      end
    end

    def system(thread, view)
      [['_sessionid', view[:session_id].to_s],
       ['_name', view.dig(:definition, :family, :name)],
       ['_ioprocessors', @expressions.value(thread, '$_ioprocessors')],
       ['now', context.now],
       ['actor', thread.handle],
       ['definition', { id: view[:definition_id], family: view.dig(:definition, :family, :name),
                        revision: view.dig(:definition, :revision) }],
       ['residency', view[:residency]],
       ['published', view[:published]],
       ['allocated_bytes', view[:allocated_bytes]]]
    end
  end
end
