# frozen_string_literal: true

module DAP
  # Expressions in an actor's data model, evaluated the way the workbench
  # shows a guard: `json.transform`, the host's jq, over the data root the
  # actor reports (`ta_actor_datamodel_root`).
  #
  # A jq program in a chart also sees the SCXML system variables and can
  # call In(). Outside a macrostep core cannot supply them, so the adapter
  # binds them itself, as literals, from what inspection and the trace
  # records say: `$_event` is the event of the step the actor is stopped
  # in, and In() tests the configuration after its last completed
  # microstep (ADR 0061 section 2).
  class Expressions
    SCXML_PROCESSOR = 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor'

    # {processor index => {type:, uri:}}: how to show an I/O origin of a
    # processor the adapter registered. Core renders origins only inside a
    # run (ADR 0068), and the host does not bind ta_origin_text.
    attr_reader :io_origins

    def initialize(session, labels, processors)
      @session = session
      @labels = labels
      # {type => location template}, `%s` standing for the session ID: the
      # I/O processors the adapter registered, for $_ioprocessors.
      @processors = processors
      @io_origins = {}
    end

    def context = @session.context

    # The actor's data root; nil when its data model has none.
    def root(thread)
      response = context.ta.actor.datamodel_root(system: context.system_handle, actor: thread.handle)
      response[:result] == 'ok' ? response.dig(:out, :out) : nil
    end

    # Every value the expression produces. Raises RequestError with jq's
    # message when it does not compile or fails.
    def evaluate(thread, expression, event: nil, input: :root)
      input = root(thread) if input == :root
      context.rpc('json.transform', filter: "#{environment(thread, event)}#{expression}", input: input)
    rescue TinyactorsTest::RPCError => error
      raise RequestError, (error.data.is_a?(String) ? error.data : error.rpc_message).to_s
    end

    # The one value of an expression, or an error that says why not.
    def value(thread, expression, event: nil)
      outputs = evaluate(thread, expression, event: event)
      raise RequestError, "#{expression} produced #{outputs.size} values, not one" unless outputs.size == 1

      outputs.first
    end

    # jq truthiness of an expression's first value: a condition.
    def holds?(thread, expression, event: nil)
      first = evaluate(thread, expression, event: event).first
      !(first.nil? || first == false)
    end

    # The path an expression names, as jq's `path()` answers it.
    def path(thread, expression, event: nil)
      value(thread, "[path(#{expression})]", event: event).first
    end

    # The value a user typed: JSON when it parses, else an expression.
    def literal(thread, text, event: nil)
      JSON.parse(text)
    rescue JSON::ParserError
      value(thread, text, event: event)
    end

    # `.a[0]["b c"]`: a path as a jq location, for evaluateName.
    def self.location(path)
      return '.' if path.empty?

      path.map do |key|
        case key
        when Integer then "[#{key}]"
        when /\A[A-Za-z_][A-Za-z0-9_]*\z/ then ".#{key}"
        else "[#{JSON.generate(key.to_s)}]"
        end
      end.join.then { |text| text.start_with?('.') ? text : ".#{text}" }
    end

    # SCXML's _event (5.10.1) from a trace record's or inspection's event.
    def scxml_event(event)
      return nil if event.nil?

      name = event[:name_text] || context.event_text(event[:name])
      invoke = event[:invoke_id].to_s == TinyactorsTest::INVOKE_ID_NONE ? nil : event[:invoke_id]
      { name: name, type: event[:type], sendid: event[:send_id].to_s == '0' ? nil : event[:send_id],
        origin: origin_text(event[:origin]), origintype: origin_type(event[:origin]), invokeid: invoke,
        data: event[:data] }
    end

    private

    def environment(thread, event)
      view = @session.inspect_actor(thread.handle)
      return '' if view.nil?

      session_id = view[:session_id].to_s
      name = view.dig(:definition, :family, :name).to_s
      states = active_names(thread, view)
      processors = { 'scxml' => { location: "#_scxml_#{session_id}" },
                     SCXML_PROCESSOR => { location: "#_scxml_#{session_id}" } }
      @processors.each { |type, template| processors[type] = { location: format(template, session_id) } }
      "def In($s): any(#{JSON.generate(states)}[]; . == $s); " \
        "#{JSON.generate(scxml_event(event))} as $_event | #{JSON.generate(session_id)} as $_sessionid | " \
        "#{JSON.generate(name)} as $_name | #{JSON.generate(processors)} as $_ioprocessors | "
    end

    def active_names(thread, view)
      response = context.ta.actor.active_states(system: context.system_handle, actor: thread.handle,
                                                capacity: view.fetch(:active_state_count))
      return [] unless response[:result] == 'ok'

      Array(response.dig(:out, :states)).map { |state| @labels.state(view[:definition_id], state) }
    end

    # The origin as text where the adapter can say it, else as core
    # reports it.
    def origin_text(origin)
      case origin&.dig(:kind)
      when nil, 'none' then nil
      when 'io' then @io_origins.dig(origin.dig(:io, :processor), :uri) || origin[:io]
      when 'actor' then origin[:actor]
      else origin
      end
    end

    def origin_type(origin)
      case origin&.dig(:kind)
      when 'io' then @io_origins.dig(origin.dig(:io, :processor), :type) || "processor #{origin.dig(:io, :processor)}"
      when 'actor' then SCXML_PROCESSOR
      end
    end
  end
end
