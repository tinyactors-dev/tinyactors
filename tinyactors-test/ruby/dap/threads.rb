# frozen_string_literal: true

module DAP
  # The threads of a debug session: every named actor the workbench runs
  # and, below each, the children its invocations started.
  #
  # Runtime IDs change when an actor is replaced or reloaded, and DAP wants
  # a thread to keep its ID, so a thread is keyed by what stays: a named
  # actor by its name, an invoked child by its runtime ID (it has no name,
  # and is never reloaded).
  class Threads
    Entry = Struct.new(:id, :key, :handle, :session, :definition, :actor, :parent, :name, :label,
                       keyword_init: true) do
      def named? = !actor.nil?
    end

    def initialize(session, labels)
      @session = session
      @labels = labels
      @ids = {}
      @entries = {}
      @next_id = 0
      @started = []
      @exited = []
    end

    def to_a = @entries.values.sort_by(&:id)

    def [](id) = @entries[Integer(id)]

    def by_session(session_id) = @entries.each_value.find { |entry| entry.session == session_id }

    def by_handle(handle) = @entries.each_value.find { |entry| entry.handle == handle }

    def by_label(label) = @entries.each_value.find { |entry| entry.label == label.to_s }

    # Re-reads the actors, and notes which threads appeared and which went
    # away for `take_changes`. Answers nil.
    def refresh
      seen = {}
      @session.actors.each do |name, actor|
        view = @session.inspect_actor(actor.handle)
        next if view.nil?

        add(seen, [:named, name], actor.handle, view, actor: actor, label: name.to_s)
        children(seen, actor.handle, view, name.to_s)
      end
      @started.concat(seen.keys - @entries.keys)
      @exited.concat((@entries.keys - seen.keys).map { |id| @entries[id] })
      @entries = seen
      nil
    end

    # [started, exited] since the last call: thread IDs that appeared, and
    # the entries of threads that went away.
    def take_changes
      changes = [@started.uniq, @exited.uniq(&:id)]
      @started = []
      @exited = []
      changes
    end

    # The names of the active states that have no active child, or nil
    # when the actor has no configuration to read.
    def leaves(handle, view)
      definition = view[:definition_id]
      response = @session.context.ta.actor.active_states(system: @session.context.system_handle, actor: handle,
                                                         capacity: view.fetch(:active_state_count))
      return nil unless response[:result] == 'ok'

      active = Array(response.dig(:out, :states))
      parents = active.filter_map { |state| @labels.state_parent(definition, state) }
      (active - parents).map { |state| @labels.state(definition, state) }
    end

    private

    def children(seen, handle, view, parent_label)
      parent = seen.each_value.find { |entry| entry.handle == handle }
      @session.child_actors(handle).each do |child|
        child_view = @session.inspect_actor(child)
        next if child_view.nil?

        family = child_view.dig(:definition, :family, :name)
        add(seen, [:child, child], child, child_view, parent: parent&.id, label: "#{parent_label}.#{family}",
                                                       family: family)
        children(seen, child, child_view, "#{parent_label}.#{family}")
      end
    end

    def add(seen, key, handle, view, actor: nil, parent: nil, label: nil, family: nil)
      id = (@ids[key] ||= (@next_id += 1))
      definition = view[:definition_id]
      where = if actor
                address = actor.address
                address ? "#{address.dig(:family, :name)}/#{address[:name]}" : label
              else
                "#{family} (actor #{handle})"
              end
      seen[id] = Entry.new(id: id, key: key, handle: handle, session: view[:session_id], definition: definition,
                           actor: actor, parent: parent, label: label,
                           name: "#{where} · #{configuration(handle, view)}")
    end

    # The leaf states of the configuration, or what the actor is instead.
    def configuration(handle, view)
      return 'done' if view[:residency] == 'done'

      leaves(handle, view)&.join(' ') || view[:residency].to_s
    end
  end
end
