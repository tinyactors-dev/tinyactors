# frozen_string_literal: true

module TinyactorsTest
  # One instrumented adapter in the host (src/instruments.c): a named
  # adapter instance that journals what core asks of it and answers with
  # faults armed beforehand. Scenarios get instruments from the Context
  # (`allocator`, `recording_router`, `scripted_invoker`, `recording_io`,
  # `memory_directory`) rather than building them.
  class Instrument
    # `name` is unique in the host process; `label` is the name within one
    # scenario run, which traces use so that two runs compare equal.
    attr_reader :name, :kind, :label

    def initialize(driver, name:, kind:, label:)
      @driver = driver
      @name = name
      @kind = kind
      @label = label
    end

    # The adapter argument that creates the instrument (first use, with
    # `config`) or refers to it again (later uses).
    def adapter(**config)
      { '$adapter' => kind, 'config' => { 'name' => name }.merge(config.transform_keys(&:to_s)) }
    end

    # Journal entries, oldest first; `since:` skips entries up to that seq.
    # Reading the journal of an instrument core has never been handed is an
    # error, so an unused instrument cannot look like a quiet one.
    def journal(since: nil)
      params = { name: name }
      params[:since] = since if since
      @driver.call('instrument.journal', params)
    end

    # Journal entries of one operation, e.g. `entries('submit')`.
    def entries(op)
      journal.select { |entry| entry[:op] == op }
    end

    # Steers the instrument. `kind` lets the host create it when core has
    # not been handed it yet, so a scenario can arm an instrument before the
    # call that first uses it.
    def control(command, **args)
      @driver.call('instrument.control', name: name, command: command, kind: kind, args: args)
    end

    # Arms a fault. Allocator: `arm_fault(after: n, count: m)`. Others:
    # `arm_fault(:submit, count: 1, result: 'err_busy', status: 'retry', message: '...')`.
    def arm_fault(op = nil, **args)
      args[:op] = op.to_s if op
      control('fail', **args)
    end

    def clear_faults = control('clear_faults')

    # allocator.counting
    def stats = control('stats')

    # Bounds what core may hold at once, as a fixed pool does (ADR 0023):
    # an allocation that would push live bytes over `bytes` fails. `bytes:
    # 0`, like `clear_faults`, lifts the budget again.
    def budget(bytes) = control('budget', bytes: bytes)

    # router.recording
    def mode(mode) = control('mode', mode: mode.to_s)
    def held = control('held')
    def release(id) = control('release', id: id)
    def drop(id) = control('drop', id: id)

    # invoker.scripted
    def services = control('services')
    def started = entries('start')

    # log.recording: the entries a chart logged, as [label, value] pairs.
    def logged = entries('log').map { |entry| [entry[:label], entry[:value]] }

    # inspector.recording: the views ta_system_visit_actors handed it.
    def visits = entries('visit')

    # directory.memory
    def directory_entries = control('entries')

    def put(family:, name:, kind:, actor: nil)
      args = { family: family, name: name, kind: kind.to_s }
      args[:actor] = actor if actor
      control('put', **args)
    end

    def to_s = "#{kind}(#{name})"
  end
end
