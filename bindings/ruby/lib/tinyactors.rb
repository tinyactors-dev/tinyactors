# frozen_string_literal: true

require "tinyactors/tinyactors"

module TinyActors
  def self.send_event(event, **options)
    { type: :send, event: event.is_a?(String) ? event.dup : event.to_s }.merge(options)
  end

  def self.raise_event(event, **options)
    { type: :raise, event: event.is_a?(String) ? event.dup : event.to_s }.merge(options)
  end

  def self.cancel(sendid)
    { type: :cancel, sendid: sendid.to_s }
  end

  def self.assign(location, value = nil, **options)
    { type: :assign, location: location.to_s, value: value }.merge(options)
  end

  class Actor
    attr_reader :system, :id

    def initialize(system, id)
      @system = system
      @id = Integer(id)
    end

    def ref
      id
    end

    def send(event, data = nil)
      system.post(Target.instance(id), event, data)
    end

    def stop
      system.actor_stop(id)
    end

    def release
      system.actor_release(id)
    end

    def snapshot
      system.actor_inspect(id)
    end
  end

  class ActorRef
    def id
      self[:id]
    end
  end

  class RequestRef
    def id
      self[:id]
    end
  end

  class Invocation
    def id
      self[:id]
    end
  end

  class Event
    def name
      self[:name]
    end

    def data
      self[:data]
    end
  end

  class System
    alias __native_initialize initialize

    def initialize(options = {})
      @configuration = options.dup
      @configuration[:ioprocessors] = Array(@configuration[:ioprocessors]).map do |processor|
        processor = processor.dup
        processor[:type] = processor[:type].to_s if processor[:type]
        processor[:aliases] = Array(processor[:aliases]).map(&:to_s)
        processor.freeze
      end.freeze
      __native_initialize(options)
    end

    def ioprocessors
      @configuration[:ioprocessors] || []
    end

    def definition_build(definition = nil, **options)
      value = definition.respond_to?(:to_h) ? definition.to_h : definition
      value = options.merge(states: value) unless value.is_a?(Hash)
      value = options if value.empty?
      __native_definition_build(value || options)
    end

    alias definition_prepare definition_build

    def actor(id)
      Actor.new(self, id)
    end

    def actor_spawn(definition, **options)
      Actor.new(self, __native_actor_spawn(definition, options))
    end
  end

  class Definition
    def name
      self[:name]
    end

    def initial
      self[:initial]
    end

    def states
      self[:states] || {}
    end
  end

  class ChartState
    def id
      self[:id]
    end
  end

  class DefinitionBuilder
    attr_reader :name, :options, :states

    def initialize(name, options = {})
      @name = name.to_sym
      @options = options.dup
      @states = {}
    end

    def initial(state)
      @options[:initial] = state.to_sym
    end

    def state(name, options = {})
      key = name.to_sym
      body = options.dup
      if block_given?
        nested = StateBuilder.new(key, body)
        yield nested
        body = nested.to_h
      end
      @states[key] = body
      body
    end

    def to_h
      @options.merge(name: @name, states: @states)
    end
  end

  class StateBuilder
    def initialize(name, body = {})
      @body = body
      @body[:id] ||= name
    end

    def on(event, target: nil, **options)
      @body[:on] ||= []
      # Event names are opaque SCXML strings. Preserve hierarchy, punctuation,
      # and case exactly; only coerce non-string name-like objects via #to_s.
      event_name = event.is_a?(String) ? event.dup : event.to_s
      transition = target.nil? ? options : options.merge(target: target)
      @body[:on] << { event: event_name, transition: transition }
    end

    def always(target: nil, **options)
      @body[:always] ||= []
      @body[:always] << (target.nil? ? options : options.merge(target: target))
    end

    def entry(action = nil, &block)
      @body[:entry] ||= []
      @body[:entry] << (action || block)
    end

    def exit(action = nil, &block)
      @body[:exit] ||= []
      @body[:exit] << (action || block)
    end

    def parallel
      @body[:parallel] = true
    end

    def history(kind = :shallow)
      @body[:history] = kind.to_sym
    end

    def final
      @body[:final] = true
    end

    def to_h
      @body
    end
  end

  def self.actor(name, **options)
    builder = DefinitionBuilder.new(name, options)
    yield builder if block_given?
    Definition.new(builder.to_h)
  end
end
