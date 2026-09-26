# frozen_string_literal: true

# An image restores into any system that registers its revision (ADR 0085).
# Logical time belongs to a system (ADR 0012), so a copy that must run on a
# clock of its own, as a debugger's fork does, lives in a second system.
#
# The scenario's own system captures; a second system in the same host,
# with its own clock and router, compiles the same chart and reconstructs
# the image. Its names are interned in its own order, so the copy works
# only if every name, send ID and I/O type came across as text. The test
# host's values belong to its data-model contexts, not to a system, so it
# passes no adopter, and a value outlives the system that made it.
module ImagesAcrossSystems
  # A second system: its own clock, a router.direct into itself, and the
  # scenario's counting allocator, so the scenario's leak check covers it.
  # `destroy` it before the scenario ends.
  class OtherSystem
    attr_reader :handle

    def initialize(context)
      @context = context
      @ta = context.ta
      @handle = context.core('ta_system_create',
                             @ta.system.create(config: { allocator: context.allocator.adapter, initial_time: 0 }))
                       .fetch(:out)
      router = { '$adapter' => 'router.direct', 'config' => { 'system' => @handle } }
      context.core('ta_system_set_external_event_router',
                   @ta.system.set_external_event_router(system: @handle, router: router))
      @definitions = {}
      @now = 0
    end

    # Compiles and registers `chart` here, once. `skew:` interns other names
    # first, so this system's IDs differ from the capturing system's.
    def define(chart, skew: [])
      @definitions[chart] ||= begin
        skew.each { |text| intern_name(text) }
        datamodel = TinyactorsTest::Context::DATAMODELS.fetch(chart.datamodel || 'jq')
        compiled = @context.core('ta_definition_compile_spec',
                                 @ta.definition.compile_spec(system: @handle, spec: chart.to_spec, datamodel: datamodel,
                                                             allocator: @context.allocator.adapter))
        @context.core('ta_system_register_definition',
                      @ta.system.register_definition(system: @handle, definition: compiled.fetch(:out))).fetch(:out)
      end
    end

    def spawn(chart, binding: nil)
      params = { system: @handle, definition: define(chart) }
      params[:datamodel_binding] = binding unless binding.nil?
      id = @context.core('ta_actor_spawn', @ta.actor.spawn(**params)).fetch(:out)
      run
      id
    end

    # ta_actor_reconstruct of an image captured anywhere; runs the copy.
    def reconstruct(image, binding: nil)
      params = { system: @handle, image: image }
      params[:datamodel_binding] = binding unless binding.nil?
      id = @context.core('ta_actor_reconstruct', @ta.actor.reconstruct(**params)).fetch(:out)
      run
      id
    end

    # Moves this system's clock alone, and runs what that makes due.
    def advance_time(milliseconds)
      @now += milliseconds
      @context.core('ta_system_set_time', @ta.system.set_time(system: @handle, now: @now))
      run
    end

    def next_deadline
      response = @ta.system.next_deadline(system: @handle)
      response[:result] == 'err_not_found' ? nil : Integer(@context.core('ta_system_next_deadline', response).fetch(:out))
    end

    def inspect_actor(id)
      @context.core('ta_actor_inspect', @ta.actor.inspect(system: @handle, actor: id)).fetch(:out)
    end

    def active(id)
      view = inspect_actor(id)
      states = @context.core('ta_actor_active_states',
                             @ta.actor.active_states(system: @handle, actor: id,
                                                     capacity: view.fetch(:active_state_count)))
      Array(states[:states]).map do |state|
        @context.core('ta_definition_state',
                      @ta.definition.state(system: @handle, definition: view.fetch(:definition_id), state: state))
                .fetch(:out).fetch(:id).to_sym
      end
    end

    def data(id)
      @context.core('ta_actor_datamodel_root', @ta.actor.datamodel_root(system: @handle, actor: id)).fetch(:out)
    end

    def event_text(name)
      @context.core('ta_event_name_format', @ta.event.name_format(system: @handle, name: name, capacity: 1024))
              .fetch(:buffer)
    end

    def send_id_text(send_id)
      @context.core('ta_send_id_format', @ta.send.id_format(system: @handle, send_id: send_id, capacity: 256))
              .fetch(:buffer)
    end

    def destroy
      @ta.system.destroy(system: @handle)
    end

    private

    def intern_name(dotted)
      atoms = dotted.split('.').map do |segment|
        @context.core('ta_system_intern_atom', @ta.system.intern_atom(system: @handle, text: segment)).fetch(:out)
      end
      @context.core('ta_event_name_intern', @ta.event.name_intern(system: @handle, segments: { atoms: atoms }))
    end

    # Resumes every actor with work and dispatches what is due, until
    # nothing is left at this system's time.
    def run
      100.times do
        runnable = @context.rpc('system.actors', system: @handle, runnable: true)
        runnable.each do |id|
          @context.core('ta_actor_resume', @ta.actor.resume(system: @handle, actor: id, budget: 1000))
        end
        next unless runnable.empty?

        due = @context.core('ta_system_dispatch_due', @ta.system.dispatch_due(system: @handle, budget: 100))
        return unless due.fetch(:dispatched).positive? || due.fetch(:more_due)
      end
      raise TinyactorsTest::ScenarioError, 'the other system did not become quiescent'
    end
  end

  # Arms a renewal 10s out with data, and one to `peer` 20s out; counts
  # renewals, and records a send that failed.
  def self.renewal
    TinyactorsTest::Chart.define(:renewal) do
      data renewals: 0, plan: nil, peer: nil, failed: nil
      state :active do
        on_entry do
          send_event 'billing.renewal.due', delay: '10s', id: 'renew', data: '{plan: "gold"}'
          send_event 'billing.report', delay: '20s', id: 'report', to_expr: '.peer'
        end
        transition on: 'billing.renewal.due', type: :internal do
          assign '.renewals', '.renewals + 1'
          assign '.plan', '$_event.data.plan'
        end
        transition on: 'error.communication', type: :internal do
          assign '.failed', '$_event.sendid'
        end
      end
    end
  end

  # Counts the reports it receives.
  def self.ledger
    TinyactorsTest::Chart.define(:ledger) do
      data reports: 0
      state :open do
        transition on: 'billing.report', type: :internal do
          assign '.reports', '.reports + 1'
        end
      end
    end
  end

  # Holds an invocation; its loss moves it to `lost`.
  def self.client
    TinyactorsTest::Chart.define(:svc_client) do
      state :using do
        invoke type: 'urn:tinyactors:test:svc', id: 'billing.svc'
        transition on: 'error.communication.invoke', target: :lost
      end
      state :lost
    end
  end

  # A <foreach> that raises an event with data on each pass, so an actor
  # paused inside it holds a snapshot and queued events with data.
  def self.looper
    TinyactorsTest::Chart.define(:looper) do
      data log: { expr: '[]' }
      state :idle do
        transition on: 'go', target: :working do
          foreach '[1, 2, 3]', item: 'i' do
            assign '.log', '.log + ["t \\(.i)"]'
            raise_event 'loop.step', data: '{n: .i}'
          end
        end
      end
      state :working do
        transition on: 'loop.step', type: :internal do
          assign '.log', '.log + ["s \\($_event.data.n)"]'
        end
      end
    end
  end

  # Other names first, so the second system numbers its atoms and event
  # names differently.
  SKEW = %w[zeta.one eta theta billing.x error.other].freeze
end

TinyactorsTest.scenarios 'images across systems' do
  # Catches: a copy whose names are the capturing system's numbers (it
  # would react to other events, or none), whose timer runs on the
  # capturing system's clock, or whose send to an actor of the capturing
  # system reaches whatever actor has that runtime ID in the second one.
  scenario 'a copy in another system runs on that system\'s clock' do
    ledger = spawn ImagesAcrossSystems.ledger
    chart = ImagesAcrossSystems.renewal
    original = spawn chart, binding: { renewals: 0, plan: nil, peer: "#_scxml_#{ledger.handle}", failed: nil }
    image = capture(original)
    other = ImagesAcrossSystems::OtherSystem.new(self)
    begin
      other.define(chart, skew: ImagesAcrossSystems::SKEW)
      bystander = other.spawn(ImagesAcrossSystems.ledger)
      assert_equal ledger.handle, bystander, 'the ledger\'s runtime ID names another actor in the second system'
      copy = other.reconstruct(image, binding: original.data)

      view = other.inspect_actor(copy)
      assert_equal copy, view.fetch(:session_id), 'a new session: the image\'s session ID is the other system\'s'
      sends = view.fetch(:delayed_sends)
      assert_equal %w[billing.renewal.due billing.report], sends.map { |send| other.event_text(send[:event]) }
      assert_equal %w[renew report], sends.map { |send| other.send_id_text(send[:send_id]) }
      assert original.inspect_actor[:delayed_sends].map { |send| send[:event] } != sends.map { |send| send[:event] },
             'the second system numbers these names differently'
      assert_equal 0, sends.last.dig(:destination, :runtime), 'the ledger of the capturing system names nothing here'
      assert_equal 10_000, other.next_deadline

      other.advance_time 10_000
      assert_equal({ renewals: 1, plan: 'gold' }, other.data(copy).slice(:renewals, :plan))
      assert_equal 0, original.data('.renewals'), 'the original waits for its own system\'s clock'
      other.advance_time 10_000
      assert_equal 'report', other.data(copy)[:failed], 'the send fails as one to a destroyed actor does'
      assert_equal 0, other.data(bystander)[:reports], 'the actor with that runtime ID here got nothing'

      advance_time 20_000
      assert_equal 1, original.data('.renewals')
      assert_equal 1, ledger.data('.reports'), 'the original\'s report reaches its ledger'
      assert_equal 1, other.data(copy)[:renewals], 'and the copy is not moved by it'
    ensure
      other.destroy
      destroy_image(image)
    end
  end

  # Catches: an image, or a copy, that depends on the system that captured
  # it: the capturing system is destroyed first, then the copy is made and
  # runs, with the host value its delayed send carries.
  scenario 'an image restores after the system that captured it is gone' do
    chart = ImagesAcrossSystems.renewal
    first = ImagesAcrossSystems::OtherSystem.new(self)
    second = ImagesAcrossSystems::OtherSystem.new(self)
    image = nil
    begin
      binding = { renewals: 0, plan: nil, peer: '#_scxml_0', failed: nil }
      image = core('ta_actor_capture',
                   ta.actor.capture(system: first.handle, actor: first.spawn(chart, binding: binding))).fetch(:out)
      first.destroy
      first = nil
      second.define(chart, skew: ImagesAcrossSystems::SKEW)
      copy = second.reconstruct(image, binding: binding)
      second.advance_time 10_000
      assert_equal({ renewals: 1, plan: 'gold' }, second.data(copy).slice(:renewals, :plan))
    ensure
      first&.destroy
      second.destroy
      destroy_image(image) if image
    end
  end

  # Catches: a paused macrostep that the copy cannot continue in another
  # system: its <foreach> snapshot, its queued events and their data, and
  # the names they carry. The copy made at each pause ends as the original.
  scenario 'a copy made at every pause of a macrostep finishes it in another system' do
    chart = ImagesAcrossSystems.looper
    baseline = spawn chart
    baseline.deliver 'go'
    expected = baseline.data('.log')
    assert_equal ['t 1', 't 2', 't 3', 's 1', 's 2', 's 3'], expected

    original = spawn chart
    assert_equal 'ok', original.post('go')
    other = ImagesAcrossSystems::OtherSystem.new(self)
    copies = 0
    begin
      other.define(chart, skew: ImagesAcrossSystems::SKEW)
      while original.resume(budget: 1000, action_budget: 1) == 'paused'
        image = capture(original)
        begin
          copy = other.reconstruct(image, binding: original.data)
          assert_equal expected, other.data(copy)[:log], "the copy made at pause #{copies + 1} ends as the original"
          assert_equal [:working], other.active(copy)
        ensure
          destroy_image(image)
        end
        copies += 1
      end
    ensure
      other.destroy
    end
    assert copies >= 6, "only #{copies} pauses"
    assert_equal expected, original.data('.log')
  end

  # Catches: an invocation ID that stays a number of the capturing system:
  # the loss is reported under a name the second system never interned.
  scenario 'a lost invocation is reported in the other system' do
    scripted_invoker('urn:tinyactors:test:svc')
    chart = ImagesAcrossSystems.client
    original = spawn chart
    image = capture(original)
    other = ImagesAcrossSystems::OtherSystem.new(self)
    begin
      other.define(chart, skew: ImagesAcrossSystems::SKEW)
      copy = other.reconstruct(image)
      assert_equal [:lost], other.active(copy), 'error.communication.invoke.billing.svc.unavailable reached it'
      assert_equal [:using], original.active
    ensure
      other.destroy
      destroy_image(image)
    end
  end

  # Catches: unload that trusts the IDs of an image of another system, where
  # the same runtime ID and epoch name another actor.
  scenario 'an image of another system cannot unload an actor here' do
    chart = ImagesAcrossSystems.renewal
    binding = { renewals: 0, plan: nil, peer: '#_scxml_0', failed: nil }
    here = spawn chart, name: 'r1', binding: binding
    other = ImagesAcrossSystems::OtherSystem.new(self)
    image = nil
    begin
      there = other.spawn(chart, binding: binding)
      assert_equal here.handle, there, 'the same runtime ID in both systems'
      image = core('ta_actor_capture', ta.actor.capture(system: other.handle, actor: there)).fetch(:out)
      assert_equal 'err_stale', unload(here, image)
    ensure
      other.destroy
      destroy_image(image) if image
    end
  end
end
