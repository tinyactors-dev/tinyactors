# frozen_string_literal: true

module DAP
  # The threads of a debug session: every named actor the workbench runs
  # and, below each, the children its invocations started; then every fork
  # (DAP::Fork), each in a system of its own, and its children.
  #
  # Runtime IDs change when an actor is replaced or reloaded, and DAP wants
  # a thread to keep its ID, so a thread is keyed by what stays: a named
  # actor by its name, a fork by its number, an invoked child by its
  # runtime ID (it has no name, and is never reloaded).
  #
  # A thread's `place` is what runs it: the workbench's Workbench::Session,
  # or a Fork. Both answer `context` (the TinyactorsTest::Context of their
  # system), `inspect_actor(handle)` and `child_actors(handle)`.
  class Threads
    Entry = Struct.new(:id, :key, :place, :handle, :session, :definition, :actor, :fork, :parent, :name, :label,
                       keyword_init: true) do
      def named? = !actor.nil?
      def fork? = !fork.nil?
      def context = place.context
      def view = place.inspect_actor(handle)
      # What identifies its session across places: runtime and session
      # IDs are numbers of one system.
      def track_key = [place.context, session]
    end

    # `forks` answers the forks, in the order they were made.
    def initialize(session, labels, forks: -> { [] })
      @session = session
      @labels = labels
      @forks = forks
      @ids = {}
      @entries = {}
      @next_id = 0
      @started = []
      @exited = []
    end

    def to_a = @entries.values.sort_by(&:id)

    def [](id) = @entries[Integer(id)]

    def by_key(key) = @entries.each_value.find { |entry| entry.track_key == key }

    def by_handle(place, handle)
      @entries.each_value.find { |entry| entry.place.equal?(place) && entry.handle == handle }
    end

    def by_label(label) = @entries.each_value.find { |entry| entry.label == label.to_s }

    # Re-reads the actors, and notes which threads appeared and which went
    # away for `take_changes`. Answers nil.
    def refresh
      seen = {}
      @session.actors.each do |name, actor|
        view = @session.inspect_actor(actor.handle)
        next if view.nil?

        add(seen, [:named, name], @session, actor.handle, view, actor: actor, label: name.to_s,
                                                                where: address_of(actor, name))
        children(seen, @session, actor.handle, view, name.to_s)
      end
      @forks.call.each do |fork|
        view = fork.inspect_actor(fork.handle)
        next if view.nil?

        add(seen, [:fork, fork.number], fork, fork.handle, view, fork: fork, label: fork.label,
                                                                  where: "fork #{fork.number} of #{fork.source_where}")
        children(seen, fork, fork.handle, view, fork.label)
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
    def leaves(place, handle, view)
      definition = Definition.new(place.context, view[:definition_id])
      context = place.context
      response = context.ta.actor.active_states(system: context.system_handle, actor: handle,
                                                capacity: view.fetch(:active_state_count))
      return nil unless response[:result] == 'ok'

      active = Array(response.dig(:out, :states))
      parents = active.filter_map { |state| @labels.state_parent(definition, state) }
      (active - parents).map { |state| @labels.state(definition, state) }
    end

    private

    def children(seen, place, handle, view, parent_label)
      parent = seen.each_value.find { |entry| entry.place.equal?(place) && entry.handle == handle }
      place.child_actors(handle).each do |child|
        child_view = place.inspect_actor(child)
        next if child_view.nil?

        family = child_view.dig(:definition, :family, :name)
        add(seen, [:child, place.context, child], place, child, child_view, parent: parent&.id,
                                                                           label: "#{parent_label}.#{family}",
                                                                           where: "#{family} (actor #{child})")
        children(seen, place, child, child_view, "#{parent_label}.#{family}")
      end
    end

    def address_of(actor, name)
      address = actor.address
      address ? "#{address.dig(:family, :name)}/#{address[:name]}" : name.to_s
    end

    def add(seen, key, place, handle, view, where:, actor: nil, fork: nil, parent: nil, label: nil)
      id = (@ids[key] ||= (@next_id += 1))
      seen[id] = Entry.new(id: id, key: key, place: place, handle: handle, session: view[:session_id],
                           definition: Definition.new(place.context, view[:definition_id]), actor: actor,
                           fork: fork, parent: parent, label: label,
                           name: "#{where} · #{configuration(place, handle, view)}")
    end

    # The leaf states of the configuration, or what the actor is instead.
    def configuration(place, handle, view)
      return 'done' if view[:residency] == 'done'

      leaves(place, handle, view)&.join(' ') || view[:residency].to_s
    end
  end
end
