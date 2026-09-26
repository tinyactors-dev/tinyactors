# frozen_string_literal: true

module DAP
  # The threads of a debug session: every named actor the workbench runs
  # and, below each, the children its invocations started.
  #
  # Runtime IDs change when an actor is replaced or reloaded, and DAP wants
  # a thread to keep its ID, so a thread is keyed by what stays: a named
  # actor by its name, an invoked child by its runtime ID (it has no name,
  # and is never reloaded).
  #
  # A thread's `place` is the workbench's Workbench::Session, which answers
  # `context` (the TinyactorsTest::Context of its system),
  # `inspect_actor(handle)` and `child_actors(handle)`.
  class Threads
    # `label` names the thread in the console's `@actor` (`acct1`,
    # `boss.worker`); `title` names it in what the adapter writes, as napi's
    # adapter does (`counter/acct1`, `supervisor/boss invocation worker`);
    # `name` is the title and the leaf states, as the Threads pane shows it.
    Entry = Struct.new(:id, :key, :place, :handle, :session, :definition, :actor, :parent, :name, :label, :title,
                       keyword_init: true) do
      def named? = !actor.nil?
      def context = place.context
      def view = place.inspect_actor(handle)
      # What identifies its session: its session ID, which survives a
      # reload (ADR 0066).
      def track_key = session
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

    def by_key(key) = @entries.each_value.find { |entry| entry.track_key == key }

    def by_handle(handle) = @entries.each_value.find { |entry| entry.handle == handle }

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
      context = @session.context
      definition = Definition.new(context, view[:definition_id])
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
        invoke = Array(view[:invokes]).find { |candidate| Integer(candidate[:service_handle]) == child }
        where = invoke ? "#{parent&.title} invocation #{place.context.invoke_id_text(invoke[:id])}" : "#{family} (actor #{child})"
        add(seen, [:child, child], place, child, child_view, parent: parent&.id,
                                                                           label: "#{parent_label}.#{family}",
                                                                           where: where)
        children(seen, place, child, child_view, "#{parent_label}.#{family}")
      end
    end

    def address_of(actor, name)
      address = actor.address
      address ? "#{address.dig(:family, :name)}/#{address[:name]}" : name.to_s
    end

    def add(seen, key, place, handle, view, where:, actor: nil, parent: nil, label: nil)
      id = (@ids[key] ||= (@next_id += 1))
      seen[id] = Entry.new(id: id, key: key, place: place, handle: handle, session: view[:session_id],
                           definition: Definition.new(place.context, view[:definition_id]), actor: actor,
                           parent: parent, label: label, title: where,
                           name: "#{where} · #{configuration(place, handle, view)}")
    end

    # The leaf states of the configuration, or what the actor is instead.
    def configuration(place, handle, view)
      return 'done' if view[:residency] == 'done'

      leaves(handle, view)&.join(' ') || view[:residency].to_s
    end
  end
end
