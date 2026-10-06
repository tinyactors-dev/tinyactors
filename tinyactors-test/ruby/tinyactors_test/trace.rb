# frozen_string_literal: true

module TinyactorsTest
  # Records what a scenario run can observe after every step, so two runs of
  # the same scenario body can be compared step by step. A step is one
  # spawn, advance_time, or deliver, taken at quiescence.
  #
  # Each step holds, for every resident actor (invoked children included):
  # the active configuration, residency, the data-model root, completion,
  # pending delayed sends, and invocations; plus the new journal entries of
  # every recording instrument (router, invokers, I/O processors, directory).
  #
  # Runtime IDs differ between runs, so actors appear under labels: a
  # spawned actor as "<chart>.<n>", any other as "actor.<n>" in the order it
  # is first seen. Runtime actor IDs inside journal entries are replaced by
  # these labels. Journal `seq` numbers are dropped.
  class Trace
    Step = Data.define(:index, :label, :time, :state) do
      def describe = "step #{index} (#{label}, t=#{time}ms)"
    end

    attr_reader :steps

    def initialize(context)
      @context = context
      @steps = []
      @journal_cursors = {}
    end

    # Everything a trace step records about one resident actor.
    def observe(id)
      actor_state(id)
    end

    def record(label)
      state = { actors: actors, journals: journals }
      @steps << Step.new(index: @steps.size, label: label, time: @context.now, state: state)
    end

    # Compares two traces; nil when they are equal, else a message that
    # names the first differing step and the path of the first difference.
    def self.difference(expected, actual, expected_name: 'baseline', actual_name: 'variant')
      expected.steps.zip(actual.steps).each do |left, right|
        if right.nil?
          return "#{actual_name} stopped after #{actual.steps.size} steps; #{expected_name} continued with #{left.describe}"
        end
        if left.label != right.label || left.time != right.time
          return "#{actual_name} ran #{right.describe} where #{expected_name} ran #{left.describe}"
        end

        path, want, got = first_difference(left.state, right.state)
        next if path.nil?

        return "#{actual_name} diverges from #{expected_name} at #{left.describe}:\n" \
               "#{path.join('.')}\n  #{expected_name}: #{want.inspect}\n  #{actual_name}: #{got.inspect}"
      end
      return nil if actual.steps.size == expected.steps.size

      extra = actual.steps[expected.steps.size]
      "#{actual_name} ran #{extra.describe} after #{expected_name} had finished"
    end

    MISSING = Object.new.tap { |missing| def missing.inspect = '(absent)' }.freeze

    # [path, expected, actual] of the first difference, or nil.
    def self.first_difference(expected, actual, path = [])
      return nil if expected == actual

      if expected.is_a?(Hash) && actual.is_a?(Hash)
        (expected.keys | actual.keys).each do |key|
          found = first_difference(expected.fetch(key, MISSING), actual.fetch(key, MISSING), path + [key])
          return found if found
        end
      elsif expected.is_a?(Array) && actual.is_a?(Array) && expected.size == actual.size
        expected.zip(actual).each_with_index do |(left, right), index|
          found = first_difference(left, right, path + [index])
          return found if found
        end
      end
      [path, expected, actual]
    end

    private

    def ta = @context.ta
    def system = @context.system_handle

    def actors
      ids = @context.rpc('system.actors', system: system)
      ids.to_h { |id| [@context.actor_label(id), actor_state(id)] }.sort.to_h
    end

    def actor_state(id)
      inspection = @context.core('ta_actor_inspect', ta.actor.inspect(system: system, actor: id)).fetch(:out)
      definition = inspection.fetch(:definition_id)
      states = @context.core('ta_actor_active_states',
                             ta.actor.active_states(system: system, actor: id,
                                                    capacity: inspection.fetch(:active_state_count)))
      {
        definition: inspection.dig(:definition, :family, :name),
        residency: inspection[:residency],
        published: inspection[:published],
        macrostep_in_progress: inspection[:macrostep_in_progress],
        internal_events: Array(inspection[:internal_events]).map { |queued| @context.event_text(queued.dig(:event, :name)) },
        active: Array(states[:states]).map { |state| @context.state_name(definition, state) },
        data: data(id),
        completion: completion(id, inspection),
        delayed_sends: delayed_sends(inspection),
        invokes: Array(inspection[:invokes]).map { |invoke| invoke.except(:service_handle) }
      }
    end

    def data(id)
      response = ta.actor.datamodel_root(system: system, actor: id)
      response[:result] == 'ok' ? response.dig(:out, :out) : { unavailable: response[:result] }
    end

    def completion(id, inspection)
      return nil unless inspection[:has_completion]

      out = @context.core('ta_actor_get_completion', ta.actor.get_completion(system: system, actor: id)).fetch(:out)
      { data: out[:has_data] ? out[:data] : nil }
    end

    # Inspection carries the actor's pending delayed sends, by due time (ADR 0054).
    def delayed_sends(inspection)
      Array(inspection[:delayed_sends]).map do |send|
        { due: send[:due], event: send[:event].to_i.zero? ? send[:event_text] : @context.event_text(send[:event]),
          send_id: send[:send_id],
          target: @context.send_target(inspection[:definition_id], send), data: send[:data] }
      end
    end

    # Trace records a run writes when it pauses, resumes, or stops for lack
    # of memory: the only ones that may differ between runs that do the same
    # work (ADR 0067), so a Trace leaves them out.
    CONTROL_RECORDS = %w[paused resumed stopped].freeze

    def journals
      @context.instruments.each_value.filter_map do |instrument|
        next if instrument.kind == 'allocator.counting'

        entries = instrument.journal(since: @journal_cursors[instrument.name])
        @journal_cursors[instrument.name] = entries.last[:seq] unless entries.empty?
        if instrument.kind == 'trace.recording'
          entries = entries.reject { |entry| CONTROL_RECORDS.include?(entry[:kind]) }
        end
        [instrument.label, entries.map { |entry| @context.label_actors(entry.except(:seq)) }]
      end.to_h
    end
  end
end
