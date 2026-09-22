# frozen_string_literal: true

module Workbench
  # The live system: one host process, one ta_system, and one named actor per
  # declaration in the workbench directory. Neither the host nor the system is
  # ever torn down. A chart edit is applied with the core replacement protocol
  # (ta_actor_prepare_replacement / ta_actor_activate_replacement), which is
  # why every top-level actor is a named actor: only a published named actor
  # can be replaced, and only the directory entry makes the swap atomic.
  class Session
    # Reloading a chart replaces the actor with a fresh session of the new
    # definition at its initial configuration, bound to the data root the
    # incumbent had. Its invoked children do not survive: an invoke belongs to
    # the state that started it, and the replacement has not entered any.
    class ReplacementFailed < StandardError; end

    attr_reader :context, :sources, :journal, :actors, :focus
    # The Clock driving logical time, when the App installed one. The panes
    # show its rate; the Session only reads it.
    attr_accessor :clock

    def initialize(driver:, sources:, journal:)
      @driver = driver
      @sources = sources
      @journal = journal
      @group = TinyactorsTest::Group.new('workbench', chart: nil)
      # The Context reads TinyactorsTest.charts by reference, so a reload that
      # replaces a Chart object gives the next definition lookup the new spec.
      @context = TinyactorsTest::Context.new(@driver, @group, TinyactorsTest.charts, run: 'workbench')
      @actors = {}
      @log_cursor = nil
      @focus = nil
      @generation = 0
    end

    # Bumped whenever the system changes, so panes redraw only when needed.
    attr_reader :generation

    def touch! = @generation += 1

    # Installs the directory the named actors claim their addresses in and the
    # sink the charts' <log> goes to, then spawns the declared actors. Both
    # instruments create the system on first use.
    def start!
      @context.memory_directory
      @context.recording_log
      report_source_errors
      sync_actors!
      touch!
    end

    # ------------------------------------------------------------------
    # Reloading

    # Re-reads the directory and applies it: every actor whose chart changed
    # is replaced in place, actors that were added are spawned, and actors no
    # longer declared are destroyed. The system keeps running throughout.
    def reload!
      changed = @sources.reload!
      report_source_errors
      refresh_invoker!

      replaced = []
      # A snapshot: replacing an actor rewrites this table, and respawning one
      # removes and re-adds its entry.
      @actors.to_a.each do |name, incumbent|
        spec = @sources.actors[name]
        next if spec.nil?
        next unless changed.include?(spec.chart)

        # The chart's file failed to load. Leaving the actor running is the
        # whole point of a workbench: a typo should cost a reload, not the
        # state the session has built up.
        unless TinyactorsTest.charts.key?(spec.chart)
          @journal.warn("#{name}: chart #{spec.chart} did not load; kept the running actor")
          next
        end

        replaced << name if replace_actor(name, incumbent, spec)
      end
      sync_actors!

      if changed.empty?
        @journal.info('reload: no chart changed')
      else
        @journal.info("reload: #{changed.sort.join(', ')} changed; " \
                      "#{replaced.empty? ? 'no actor replaced' : "replaced #{replaced.sort.join(', ')}"}")
      end
      touch!
      drain_chart_logs
      replaced
    end

    # Replaces one named actor with a session of its chart's new definition,
    # carrying its data root across. Returns true when the swap happened.
    def replace_actor(name, incumbent, spec)
      unless same_family?(incumbent, spec)
        return respawn_actor(name, incumbent, spec,
                             'its definition family changed, which the replacement protocol cannot span')
      end

      carried = data_of(incumbent)
      old_definition = incumbent.definition
      children = child_actors(incumbent.handle)
      was_active = active_states(incumbent)
      dropped = inspect_actor(incumbent.handle)&.dig(:delayed_send_count).to_i

      replacement = @context.prepare_replacement(incumbent, spec.chart, binding: carried)
      result = @context.activate_replacement(incumbent, replacement)
      unless result == 'ok'
        # The replacement was never published, so the incumbent still owns the
        # address and keeps running; drop the prepared session.
        destroy_tree(replacement.handle)
        @context.forget(replacement)
        @journal.error("#{name}: activate_replacement answered #{result}; kept the running actor")
        return false
      end

      # The incumbent no longer holds the address. Its invoked children are
      # separate actors that ta_actor_destroy does not cascade into, so they
      # are reaped explicitly before it goes.
      children.each { |child| destroy_tree(child) }
      destroy_tree(incumbent.handle)
      @context.forget(incumbent)
      @actors[name] = replacement
      retire(old_definition, replacement.definition)
      @context.run_system("replaced #{name}")
      @journal.info("#{name}: replaced (definition #{old_definition} -> #{replacement.definition}" \
                    "#{carried.nil? ? '' : ', data carried'}#{children.empty? ? '' : ", #{children.size} child(ren) reaped"})")
      report_lost_progress(name, was_active, active_states(replacement), dropped)
      true
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError, TinyactorsTest::ScenarioError => error
      @journal.exception("#{name}: replacement failed", error)
      false
    end

    # A replacement is a fresh interpreter: core does not translate a
    # configuration from one definition revision to another, and the delayed
    # sends the incumbent owned die with it. Both are invisible from the
    # outside, so the actor simply stops, which is worth saying out loud.
    # A chart carries itself across an upgrade by keeping what it needs in
    # the data model, which is carried, and resuming from it.
    def report_lost_progress(name, was_active, now_active, dropped)
      return if was_active.sort == now_active.sort && dropped.zero?

      moved = was_active.sort == now_active.sort ? nil : "was #{was_active.join(' ')}, now #{now_active.join(' ')}"
      sends = dropped.positive? ? "#{dropped} delayed send(s) dropped" : nil
      @journal.warn("#{name}: #{[moved, sends].compact.join('; ')} " \
                    '- a replacement starts at the chart\'s initial state')
    end

    # A chart whose family changed cannot be a replacement for the incumbent,
    # because the address names the family. Destroying and respawning is the
    # only way through; the data root is carried by hand.
    def respawn_actor(name, incumbent, spec, why)
      unless TinyactorsTest.charts.key?(spec.chart)
        @journal.error("#{name}: chart #{spec.chart} is not defined; kept the running actor")
        return false
      end

      carried = data_of(incumbent)
      remove_actor(name)
      @actors[name] = @context.spawn(spec.chart, name: name.to_s, binding: carried || spec.binding)
      @journal.warn("#{name}: respawned rather than replaced, because #{why}")
      true
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError, TinyactorsTest::ScenarioError => error
      @journal.exception("#{name}: respawn failed", error)
      false
    end

    # Spawns newly declared actors and destroys ones no longer declared.
    def sync_actors!
      (@actors.keys - @sources.actors.keys).each do |name|
        remove_actor(name)
        @journal.info("#{name}: no longer declared; destroyed")
      end

      @sources.actors.each do |name, spec|
        next if @actors.key?(name)

        spawn_actor(name, spec)
      end
      @focus = @actors.keys.first unless @actors.key?(@focus)
      touch!
    end

    def spawn_actor(name, spec)
      unless TinyactorsTest.charts.key?(spec.chart)
        @journal.error("#{name}: chart #{spec.chart} is not defined in #{@sources.dir}")
        return nil
      end

      actor = @context.spawn(spec.chart, name: name.to_s, binding: spec.binding)
      @actors[name] = actor
      @journal.info("#{name}: spawned #{spec.chart} at #{address_text(actor)}")
      drain_chart_logs
      actor
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError, TinyactorsTest::ScenarioError => error
      @journal.exception("#{name}: spawn failed", error)
      nil
    end

    # Destroys a named actor and its invoked children, and releases its
    # address so the name can be claimed again.
    def remove_actor(name)
      actor = @actors.delete(name)
      return if actor.nil?

      child_actors(actor.handle).each { |child| destroy_tree(child) }
      destroy_tree(actor.handle)
      @context.forget(actor)
      @context.run_system("destroy #{name}")
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError => error
      @journal.exception("#{name}: destroy failed", error)
    end

    # ------------------------------------------------------------------
    # Steps

    def deliver(name, event, data: nil)
      actor = fetch(name)
      actor.deliver(event, data: data)
      @journal.info("#{name} <- #{event}#{data.nil? ? '' : " #{JSON.generate(data)}"}")
      after_step
    end

    def advance(milliseconds)
      @context.advance_time(milliseconds)
      @journal.info("advanced to t=#{@context.now}ms")
      after_step
    end

    # One step of a running clock. Like `advance`, but it does not journal the
    # step itself: at several ticks a second that would bury everything the
    # charts have to say. What the charts log still comes through.
    def tick(milliseconds)
      @context.advance_time(milliseconds)
      after_step
    end

    # Advances from one pending deadline to the next, `steps` times or until
    # nothing is pending, which is how a chart driven by delayed sends is
    # walked forward without guessing at durations.
    def run(steps: 32)
      moved = 0
      steps.times do
        deadline = @context.next_deadline
        break if deadline.nil?

        @context.advance_time([deadline - @context.now, 0].max)
        moved += 1
      end
      if moved.zero?
        @journal.info('run: no delayed send is pending')
      else
        @journal.info("run: took #{moved} deadline(s) to t=#{@context.now}ms")
      end
      after_step
    end

    def focus=(name)
      @focus = name.to_sym
      touch!
    end

    def fetch(name)
      @actors.fetch(name.to_sym) { raise ArgumentError, "no actor named #{name}" }
    end

    def now = @context.now

    def next_deadline
      @context.next_deadline
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError
      nil
    end

    # ------------------------------------------------------------------
    # Observation

    # What every named actor and its invoked children currently look like.
    def overview
      @actors.filter_map do |name, actor|
        view = inspect_actor(actor.handle)
        next nil if view.nil?

        {
          name: name,
          chart: actor.chart,
          address: address_text(actor),
          definition: actor.definition,
          revision: revision_text(view),
          residency: view[:residency],
          published: view[:published],
          running: view[:macrostep_in_progress],
          active: active_states(actor),
          internal: Array(view[:internal_events]).size,
          sends: view[:delayed_send_count],
          children: child_actors(actor.handle).map { |child| child_view(child) }
        }
      end
    end

    # What the actor's definition listens for: one row per transition that
    # names an event, with the state it sits in, its guard, and its targets.
    #
    # The rows come from the definition core is actually running
    # (ta_definition_export, which ADR 0042 makes faithful), not from the
    # chart file on disk: after a reload that failed those two differ, and
    # only the first one is the truth about the actor in front of you.
    #
    # `active` marks a transition whose state is in the current
    # configuration. `holds` is the guard evaluated against the data root
    # right now, or nil when there is no guard or it cannot be evaluated
    # outside a macrostep (one reading `$_event`, say).
    def events(name)
      actor = fetch(name)
      spec = @context.export(actor.definition)
      active = active_states(actor).map(&:to_s)
      rows = transition_rows(Array(spec[:states]), active)
      root = data_of(actor)
      rows.each { |row| row[:holds] = guard_holds(row[:condition], root) if row[:active] }
      rows
    end

    # The focused actor's data-model root, or nil.
    def focused_data
      actor = @actors[@focus]
      return nil if actor.nil?

      data_of(actor)
    end

    # New <log> records since the last drain, appended to the journal so the
    # logs pane shows them in step with everything else.
    def drain_chart_logs
      sink = @context.instruments['log']
      return if sink.nil?

      entries = sink.journal(since: @log_cursor).select { |entry| entry[:op] == 'log' }
      return if entries.empty?

      @log_cursor = entries.last[:seq]
      entries.each do |entry|
        who = name_of_handle(entry[:source].is_a?(Hash) ? entry[:source][:actor] : entry[:source])
        value = entry[:has_value] ? " #{JSON.generate(entry[:value])}" : ''
        @journal.chart("#{who ? "#{who} " : ''}<log> #{entry[:label]}#{value}")
      end
      touch!
    rescue TinyactorsTest::RpcError
      nil
    end

    private

    def after_step
      drain_chart_logs
      touch!
    end

    def transition_rows(states, active, out = [])
      states.each do |state|
        id = state[:id].to_s
        Array(state[:transitions]).each do |transition|
          # An eventless transition is not something anyone can send.
          next if transition[:event].nil?

          out << { state: id, active: active.include?(id), event: transition[:event].to_s,
                   condition: transition[:condition],
                   targets: Array(transition[:targets]).map(&:to_s) }
        end
        transition_rows(Array(state[:states]), active, out)
      end
      out
    end

    # jq truthiness of a guard against the data root. nil when there is no
    # guard, no root, or the expression needs a macrostep's variables.
    def guard_holds(condition, root)
      return nil if condition.nil? || root.nil?

      outputs = @context.rpc('json.transform', filter: condition, input: root)
      value = outputs.first
      !(value.nil? || value == false)
    rescue TinyactorsTest::RpcError, TinyactorsTest::ProtocolError
      nil
    end

    def report_source_errors
      @sources.errors.each do |file, error|
        @journal.exception(File.basename(file), error)
      end
    end

    # Registering an invoker for a type replaces the previous registration
    # (ta_system_register_invoker), so the SCXML invoker's chart table can be
    # refreshed rather than staying as it was when the system was created.
    def refresh_invoker!
      system = @context.system_handle
      type = @context.core('ta_system_intern_atom',
                           @context.ta.system.intern_atom(
                             system: system, text: TinyactorsTest::Context::SCXML_INVOKER_TYPE
                           )).fetch(:out)
      charts = TinyactorsTest.charts.to_h { |name, chart| [name.to_s, chart.to_spec] }
      invoker = { '$adapter' => 'invoker.scxml', 'config' => { 'system' => system, 'charts' => charts } }
      @context.core('ta_system_register_invoker',
                    @context.ta.system.register_invoker(system: system, type: type, invoker: invoker))
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::RpcError => error
      @journal.exception('could not refresh the SCXML invoker', error)
    end

    def same_family?(incumbent, spec)
      current = incumbent.address&.dig(:family, :name)
      return false if current.nil?

      current == @context.family(spec.chart)
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::ScenarioError
      false
    end

    def retire(old_definition, new_definition)
      return if old_definition.nil? || old_definition == new_definition

      # Retiring is safe while the actor that used it is gone: core frees the
      # definition once no resident actor holds it.
      @context.retire_definition(old_definition)
    end

    # For invoker.scxml the service handle IS the child's actor ID
    # (src/adapters.c sets *params->service_handle = child), which is the only
    # invoker this workbench registers.
    def child_actors(handle)
      view = inspect_actor(handle)
      return [] if view.nil?

      Array(view[:invokes]).filter_map do |invoke|
        next nil unless invoke[:active]

        Integer(invoke[:service_handle])
      end
    rescue TypeError, ArgumentError
      []
    end

    # Destroys an actor and everything it invoked, deepest first.
    def destroy_tree(handle)
      child_actors(handle).each { |child| destroy_tree(child) }
      @context.ta.actor.destroy(system: @context.system_handle, actor: handle)
    end

    def inspect_actor(handle)
      response = @context.ta.actor.inspect(system: @context.system_handle, actor: handle)
      response[:result] == 'ok' ? response.dig(:out, :out) : nil
    rescue TinyactorsTest::RpcError
      nil
    end

    def active_states(actor)
      actor.active
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::ScenarioError, TinyactorsTest::RpcError
      []
    end

    def child_view(handle)
      view = inspect_actor(handle)
      return { handle: handle, active: [], residency: 'gone' } if view.nil?

      definition = view[:definition_id]
      states = @context.core('ta_actor_active_states',
                             @context.ta.actor.active_states(system: @context.system_handle,
                                                             actor: handle,
                                                             capacity: view.fetch(:active_state_count)))
      { handle: handle,
        family: view.dig(:definition, :family, :name),
        residency: view[:residency],
        active: Array(states[:states]).map { |state| @context.state_name(definition, state) } }
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::ScenarioError, TinyactorsTest::RpcError
      { handle: handle, active: [], residency: 'unknown' }
    end

    def data_of(actor)
      actor.data
    rescue TinyactorsTest::ScenarioError, TinyactorsTest::RpcError
      # A chart on the null data model has no root to carry.
      nil
    end

    def address_text(actor)
      address = actor.address
      return "runtime #{actor.handle}" if address.nil?

      "#{address.dig(:family, :name)}/#{address[:name]}"
    end

    def revision_text(view)
      revision = view.dig(:definition, :revision)
      return '-' if revision.nil?

      text = revision.is_a?(String) ? revision : JSON.generate(revision)
      text.delete('^0-9a-zA-Z')[0, 8]
    end

    def name_of_handle(handle)
      return nil if handle.nil?

      handle = Integer(handle) rescue (return nil)
      @actors.each { |name, actor| return name if actor.handle == handle }
      "actor #{handle}"
    end
  end
end
