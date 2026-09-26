# frozen_string_literal: true

module DAP
  # A copy of a workbench actor in a system of its own (core ADR 0085), for
  # trying things on real state without touching the actor: it has its own
  # clock, so `clock +3d` moves only the fork's time, and stub I/O, so its
  # sends are shown in the console and reach nothing. A fork cannot reach
  # the workbench's actors: a runtime reference to another actor names
  # nothing in its system, and its router has no directory to resolve an
  # address with.
  #
  # A fork also keeps images of itself: one at each stop of its thread and
  # one when it was made (`history`), and one when its actor last began a
  # macrostep (`frame_start`). Restoring one of them is how stepBack,
  # reverseContinue and restartFrame go back. Logical time never goes back
  # in a system (ADR 0012), so a restore builds a fresh system that starts
  # at the image's time and reconstructs the image there; the fork keeps its
  # number and its thread.
  #
  # A Fork is a place (Threads): it answers `context`, `inspect_actor` and
  # `child_actors` for its system.
  class Fork
    # An image of the fork's actor, the data root it had, and the fork
    # system's time then. `reason` is why the system stopped there.
    Snapshot = Data.define(:image, :data, :now, :reason)

    attr_reader :number, :context, :handle, :source_key, :source_where, :label, :history

    # `source` is the workbench thread to copy. `setup` is called with the
    # fork each time it builds a system, before the image is restored, to
    # install the adapter's monitor and console there; `gone` with the
    # Context of each system it destroys.
    def initialize(number:, source:, driver:, output:, setup:, gone:)
      @number = number
      @driver = driver
      @output = output
      @setup = setup
      @gone = gone
      @source_key = source.key
      @source_where = source.title
      @label = "#{source.label}.fork#{number}"
      @generation = 0
      @history = []
      @frame_start = nil
      copy(source)
    end

    # ------------------------------------------------------------------
    # A place

    def inspect_actor(handle)
      response = @context.ta.actor.inspect(system: @context.system_handle, actor: handle)
      response[:result] == 'ok' ? response.dig(:out, :out) : nil
    end

    # For invoker.scxml the service handle is the child's actor ID, as in
    # the workbench (Workbench::Session#child_actors).
    def child_actors(handle)
      view = inspect_actor(handle)
      return [] if view.nil?

      Array(view[:invokes]).filter_map { |invoke| invoke[:active] ? Integer(invoke[:service_handle]) : nil }
    end

    # ------------------------------------------------------------------
    # Images

    # Called by the Control before one of the fork's actors begins a
    # macrostep: the image restartFrame goes back to.
    def macrostep_starting(handle)
      return unless handle == @handle

      release(@frame_start)
      @frame_start = snapshot_of('restart')
    end

    # Keeps an image of where the fork stopped, for stepBack.
    def snapshot(reason)
      @history << snapshot_of(reason)
    end

    # Goes back to the image before the last stop. Answers its reason.
    def step_back
      raise RequestError, "#{@label} has no earlier stop to step back to" if @history.size < 2

      release(@history.pop)
      restore(@history.last)
      @history.last.reason
    end

    # Goes back to the last earlier stop at a breakpoint or an exception,
    # or to where the fork began. Answers the reason it stopped for there.
    def reverse_continue
      raise RequestError, "#{@label} has no earlier stop to go back to" if @history.size < 2

      earlier = @history[0...-1]
      index = earlier.rindex { |snapshot| BREAKS.include?(snapshot.reason) } || 0
      @history.pop(@history.size - index - 1).each { |snapshot| release(snapshot) }
      restore(@history.last)
      @history.last.reason
    end

    BREAKS = ['breakpoint', 'function breakpoint', 'data breakpoint', 'exception'].freeze

    # Goes back to where the current macrostep began.
    def restart_frame
      raise RequestError, "#{@label} has not begun a macrostep since it was made" if @frame_start.nil?

      restore(@frame_start)
      @history << snapshot_of('restart')
    end

    # A new copy of the source actor, as it is now.
    def restart(source)
      @history.each { |snapshot| release(snapshot) }
      @history = []
      release(@frame_start)
      @frame_start = nil
      destroy_system
      copy(source)
    end

    # Writes the fork actor's data root: an image of it is reconstructed
    # with the new root as its binding, in the same system, and continues
    # its session where it stood.
    def rewrite(root)
      image = capture
      destroy_actor
      @handle = reconstruct(image, root)
    ensure
      destroy_image(image) if image
    end

    # ------------------------------------------------------------------
    # Stub I/O

    # The sends the fork made since the last call, shown in the console.
    # Its sends to itself are its own business; the rest went nowhere.
    def stub_sends
      entries = @router.journal(since: @router_cursor).select { |entry| entry[:op] == 'submit' }
      return if entries.empty?

      @router_cursor = entries.last[:seq]
      entries.each do |entry|
        destination = entry[:destination] || {}
        next if destination[:kind] == 'runtime' && Integer(destination[:runtime]) == @handle

        @output.call('console', "#{@source_where} (fork #{@number}) sent #{entry.dig(:event, :name_text)} to #{describe(destination)}: " \
                                'not delivered, a fork reaches nothing outside itself')
      end
    end

    # What the fork's charts logged since the last call.
    def drain_logs
      entries = @log.journal(since: @log_cursor).select { |entry| entry[:op] == 'log' }
      return if entries.empty?

      @log_cursor = entries.last[:seq]
      entries.each do |entry|
        @output.call('stdout', Console.log_text(entry))
      end
    end

    def close
      @history.each { |snapshot| release(snapshot) }
      @history = []
      release(@frame_start)
      @frame_start = nil
      destroy_system
    end

    private

    # Captures the source actor and reconstructs it in a new system.
    def copy(source)
      source_context = source.context
      view = source.view or raise RequestError, "#{source.title} is not resident"
      @spec = source_context.core('ta_definition_export',
                                  source_context.ta.definition.export(system: source_context.system_handle,
                                                                      definition: view[:definition_id])).fetch(:out)
      image = source_context.core('ta_actor_capture',
                                  source_context.ta.actor.capture(system: source_context.system_handle,
                                                                  actor: source.handle)).fetch(:out)
      data = datamodel_root(source_context, source.handle)
      start = Snapshot.new(image: image, data: data, now: source_context.now, reason: 'entry')
      restore(start)
      @history << start
    end

    # Builds a fresh system at the snapshot's time and reconstructs its
    # image there.
    def restore(snapshot)
      destroy_system
      @generation += 1
      group = TinyactorsTest::Group.new(@label, chart: nil)
      @context = TinyactorsTest::Context.new(@driver, group, TinyactorsTest.charts,
                                             run: "dap.#{@label}.#{@generation}", initial_time: snapshot.now)
      @context.recording_trace
      @log = @context.recording_log
      @log_cursor = nil
      @router = @context.recording_router(mode: :direct)
      @router_cursor = nil
      @definition = register(@spec)
      @setup.call(self)
      @handle = reconstruct(snapshot.image, snapshot.data)
      @context.scheduler.wake
    end

    def register(spec)
      datamodel = TinyactorsTest::Context::DATAMODELS.fetch(spec[:datamodel] || TinyactorsTest::Context::DEFAULT_DATAMODEL)
      compiled = @context.core('ta_definition_compile_spec',
                               @context.ta.definition.compile_spec(system: @context.system_handle, spec: spec,
                                                                   datamodel: datamodel,
                                                                   allocator: @context.allocator.adapter))
      @context.core('ta_system_register_definition',
                    @context.ta.system.register_definition(system: @context.system_handle,
                                                           definition: compiled.fetch(:out))).fetch(:out)
    end

    # The test host's values belong to its data-model contexts, which
    # outlive every system, so no adopter is needed (ADR 0085 section 6).
    # The fork gets its own copy of the data root, as JSON.
    def reconstruct(image, data)
      params = { system: @context.system_handle, image: image }
      params[:datamodel_binding] = data unless data.nil?
      @context.core('ta_actor_reconstruct', @context.ta.actor.reconstruct(**params)).fetch(:out)
    end

    def snapshot_of(reason)
      Snapshot.new(image: capture, data: datamodel_root(@context, @handle), now: @context.now, reason: reason)
    end

    def capture
      @context.core('ta_actor_capture',
                    @context.ta.actor.capture(system: @context.system_handle, actor: @handle)).fetch(:out)
    end

    def datamodel_root(context, handle)
      response = context.ta.actor.datamodel_root(system: context.system_handle, actor: handle)
      response[:result] == 'ok' ? response.dig(:out, :out) : nil
    end

    def destroy_actor
      @context.ta.actor.destroy(system: @context.system_handle, actor: @handle)
    end

    def release(snapshot)
      destroy_image(snapshot.image) if snapshot
    end

    def destroy_image(image) = @driver.ta.session.image_destroy(image: image)

    # An image outlives the system that captured it (ADR 0085), so the
    # snapshots stay usable once the system they came from is gone.
    def destroy_system
      return if @context.nil?

      @context.ta.system.destroy(system: @context.system_handle)
      @gone.call(@context)
      @context = nil
    end

    def describe(destination)
      case destination[:kind]
      when 'named' then "#_actor_#{destination.dig(:family, :name)}/#{destination[:name]}"
      when 'runtime' then "actor #{destination[:runtime]}"
      else JSON.generate(destination)
      end
    end
  end
end
