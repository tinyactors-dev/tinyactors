# frozen_string_literal: true

# Allocation failure (ADR 0023): memory enters core through the host's
# allocator, so a host with a fixed pool, an RTOS heap, or a WASM linear
# memory can refuse. `allocator.budget(bytes)` makes that deterministic:
# an allocation that would push what core holds over the budget fails, the
# way a pool that is full does.
#
# docs/internal/errors.md decides what such a failure is: a `ta_result`
# ("API/runtime failures such as ... allocation failure"), not an SCXML
# `error.*` event. What core must never do is lose the failure.
module AllocationFailure
  # A budget that leaves `headroom` bytes: the next allocation larger than
  # that fails, whatever core is doing.
  def self.tighten(context, headroom: 32)
    context.allocator.budget(context.allocator.stats[:live_bytes] + headroom)
  end
end

TinyactorsTest.scenarios 'allocation failure', chart: :relay do
  # Compiling allocates many times, and a compiler refusing memory halfway
  # is the classic leak: the scenario refuses each allocation of one
  # compile in turn and checks that nothing is kept, whichever one it is.
  #
  # Catches: a compiler that answers ok with a half-built definition, or
  # that leaks what it had already allocated on any of its failure paths
  # (the byte count stays up, and teardown's leak check fires).
  scenario 'a compile that runs out of memory allocates nothing, wherever it fails' do
    running = spawn
    running.deliver 'go'
    spec = TinyactorsTest.charts.fetch(:traffic_light).to_spec
    jq = { '$adapter' => 'datamodel.jq' }

    # Warm the atoms and event names the chart interns in the system, then
    # measure what compiling it costs in allocations.
    warm = ta.definition.compile_spec(system: system_handle, spec: spec, datamodel: jq,
                                      allocator: allocator.adapter)
    assert_equal 'ok', warm[:result]
    destroy_definition(warm.dig(:out, :out))
    before = allocator.stats
    again = ta.definition.compile_spec(system: system_handle, spec: spec, datamodel: jq,
                                       allocator: allocator.adapter)
    assert_equal 'ok', again[:result]
    allocations = allocator.stats[:allocations] - before[:allocations]
    destroy_definition(again.dig(:out, :out))
    assert allocations > 1, "compiling allocates more than once (#{allocations})"

    allocations.times do |failing|
      at = allocator.stats
      allocator.arm_fault(after: failing, count: 1)
      result = ta.definition.compile_spec(system: system_handle, spec: spec, datamodel: jq,
                                          allocator: allocator.adapter)
      allocator.clear_faults
      assert_equal 'err_nomem', result[:result], "the compiler reports the failure of allocation #{failing + 1}"
      assert_equal at[:live_bytes], allocator.stats[:live_bytes],
                   "the failed compile kept nothing after allocation #{failing + 1} failed"
      assert_equal at[:live_allocations], allocator.stats[:live_allocations]
    end

    # The system is still a system: the actor that was already running
    # keeps running, and compiling works again.
    assert_active running, :relay, :idle
    running.deliver 'extra'
    assert_equal %w[go n=5 counted ping extra], running.data('.log')
    assert definition(:traffic_light).positive?, 'the same chart compiles once there is room'
  end

  # A spawn allocates several times (the actor record, its session
  # storage, its data-model instance), so the scenario refuses each of them
  # in turn: whichever allocation is the one that fails, the failure must
  # be clean. `arm_fault(after: n)` refuses the n+1-th allocation, which
  # names a failure point without depending on how large anything is.
  #
  # Catches: a failure path that leaves a half-built actor resident (it
  # would be visited, resumed, and counted for its definition) or that
  # keeps its memory; a spawn that reports ok after a refused allocation.
  scenario 'a spawn that runs out of memory leaves no actor behind, wherever it fails' do
    bystander = spawn
    relay = definition(:relay)
    resident = rpc('system.actors', system: system_handle)
    before = allocator.stats

    # How many allocations one spawn makes, measured rather than assumed.
    probe = spawn
    allocations = allocator.stats[:allocations] - before[:allocations]
    probe.destroy
    assert allocations.positive?, 'spawning allocates'

    allocations.times do |failing|
      at = allocator.stats
      allocator.arm_fault(after: failing, count: 1)
      assert_equal 'err_nomem', ta.actor.spawn(system: system_handle, definition: relay)[:result],
                   "spawn reports the failure of allocation #{failing + 1}"
      allocator.clear_faults

      assert_equal resident, rpc('system.actors', system: system_handle),
                   "no half-built actor is resident after allocation #{failing + 1} failed"
      assert_equal at[:live_bytes], allocator.stats[:live_bytes],
                   "the failed spawn kept nothing after allocation #{failing + 1} failed"
      assert_equal at[:live_allocations], allocator.stats[:live_allocations]
      assert_equal resident.size, definition_facts(:relay)[:actor_count],
                   'the definition counts only the actors that exist'
    end

    # The system is unharmed: spawning works again, and so does the actor
    # that was there all along.
    successor = spawn
    successor.deliver 'go'
    assert_active successor, :relay, :idle
    assert_active bystander, :relay, :idle
  end

  # A macrostep never allocates core memory (ADR 0102): core allocates what
  # a macrostep needs when it takes the input, a safe point. So a tight
  # allocator either refuses the take, and the input waits untouched with
  # no macrostep begun, or the macrostep runs to its end. Either way the
  # actor ends where an uninterrupted run does once memory is back.
  #
  # Catches: a macrostep that allocates and stops half-way, a refused take
  # that changes the actor, and a retry that repeats an effect, which the
  # comparison with the uninterrupted run would show.
  scenario 'running out of memory never interrupts a macrostep it took' do
    actor = spawn
    assert_equal 'ok', actor.post('go')

    AllocationFailure.tighten(self)
    tried = actor.run_once(budget: 1024)
    assert %w[ok err_nomem].include?(tried[:result]), "only a take may run out of memory, got #{tried[:result]}"
    allocator.clear_faults

    view = actor.inspect_actor
    if tried[:result] == 'err_nomem'
      assert !view[:macrostep_in_progress], 'a refused take begins no macrostep'
    end

    status = 'paused'
    20.times { status = actor.resume(budget: 1024); break unless status == 'paused' }
    assert_equal 'stable', status
    run_system('dispatch effects after the macrostep')
    reference = spawn
    reference.deliver 'go'
    assert_equal [reference.active, reference.data], [actor.active, actor.data],
                 'the macrostep ended where an uninterrupted one does'
  end

  # Catches: an allocator fault that core turns into a statechart event
  # instead of a result (invariant 8 draws the line: a plugin's failure has
  # an SCXML consequence only where the SCXML semantics define one, and
  # running out of memory is not an execution or communication error).
  scenario 'running out of memory is a result, not an error event' do
    recording_log
    actor = spawn
    assert_equal 'ok', actor.post('go')

    AllocationFailure.tighten(self)
    assert %w[ok err_nomem].include?(actor.run_once(budget: 1024)[:result])
    allocator.clear_faults

    assert_equal 0, actor.inspect_actor.fetch(:internal_events).to_a.size,
                   'no error.* event was queued for the chart'
    assert_equal [], logs
  end
end
