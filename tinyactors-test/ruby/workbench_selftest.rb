# frozen_string_literal: true

# Checks the workbench against the live host: that a chart edit replaces every
# named actor in place while the system keeps running, that the data root is
# carried across, that declarations added and removed are applied, that a
# broken source file is reported without taking the system down, and that
# nothing leaks once the system is destroyed.
#
#   ruby ruby/workbench_selftest.rb

require 'tmpdir'
require 'pathname'
require_relative 'workbench'

ROOT = Pathname.new(__dir__).join('..').expand_path
HOST = ROOT.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{HOST}; run `mise run build`" unless HOST.executable?

FAILURES = []

def check(what)
  result = yield
  if result
    puts "  ok   #{what}"
  else
    FAILURES << what
    puts "  FAIL #{what}"
  end
rescue StandardError => error
  FAILURES << what
  puts "  FAIL #{what}: #{error.class}: #{error.message}"
end

SUBSCRIPTION_V1 = <<~CHART
  TinyactorsTest.chart :subscription do
    data account: 'acct_1'
    data failures: 0
    initial :active
    state :active do
      transition on: 'payment.failed', target: :past_due do
        assign '.failures', '.failures + 1'
      end
    end
    state :past_due do
      transition on: 'payment.succeeded', target: :active
    end
  end
CHART

# The upgrade: a failed payment now waits in a grace period first.
SUBSCRIPTION_V2 = <<~CHART
  TinyactorsTest.chart :subscription do
    data account: 'acct_1'
    data failures: 0
    initial :active
    state :active do
      transition on: 'payment.failed', target: :dunning do
        assign '.failures', '.failures + 1'
      end
    end
    state :dunning do
      on_entry { send_event 'dunning.expired', delay: '3s', id: :dunning }
      on_exit { cancel :dunning }
      transition on: 'dunning.expired', target: :past_due
      transition on: 'payment.succeeded', target: :active
    end
    state :past_due do
      transition on: 'payment.succeeded', target: :active
    end
  end
CHART

# A different <scxml name>, so the address family changes and the replacement
# protocol cannot span it.
RENAMED = <<~CHART
  TinyactorsTest.chart :subscription, document_name: 'billing' do
    data account: 'acct_1'
    data failures: 0
    initial :active
    state :active do
      transition on: 'payment.failed', target: :holding
    end
    state :holding do
      transition on: 'payment.succeeded', target: :active
    end
  end
CHART

Dir.mktmpdir('workbench-selftest') do |tmp|
  dir = Pathname.new(tmp)
  dir.join('subscription.rb').write(SUBSCRIPTION_V1)
  dir.join('actors.rb').write("Workbench.actor :acct1, chart: :subscription\n")

  journal = Workbench::Journal.new
  sources = Workbench::Sources.new(dir)
  sources.reload!
  driver = TinyactorsTest::Driver.new(HOST, timeout: 30)
  session = Workbench::Session.new(driver: driver, sources: sources, journal: journal)

  begin
    session.start!
    system_handle = session.context.system_handle

    puts 'startup'
    check('the declared actor is spawned') { session.actors.key?(:acct1) }
    check('it is a named actor at its family address') do
      session.overview.first[:address] == 'subscription/acct1'
    end
    check('it starts in its initial state') { session.overview.first[:active].include?(:active) }

    puts 'a step'
    session.deliver(:acct1, 'payment.failed')
    check('the transition ran') { session.overview.first[:active].include?(:past_due) }
    check('the assign ran') { session.fetch(:acct1).data[:failures] == 1 }

    puts 'reloading an edited chart'
    before = session.fetch(:acct1)
    before_definition = before.definition
    before_handle = before.handle
    dir.join('subscription.rb').write(SUBSCRIPTION_V2)
    replaced = session.reload!

    check('the actor was replaced') { replaced == [:acct1] }
    check('the system was not recreated') { session.context.system_handle == system_handle }
    check('the replacement is a new session') { session.fetch(:acct1).handle != before_handle }
    check('it runs under a new definition') { session.fetch(:acct1).definition != before_definition }
    check('it kept its address') { session.overview.first[:address] == 'subscription/acct1' }
    check('it is published and resident') do
      session.overview.first[:published] && session.overview.first[:residency] == 'idle'
    end
    check('the configuration is back at the initial state') do
      session.overview.first[:active].include?(:active)
    end
    check('the data root was carried across') { session.fetch(:acct1).data[:failures] == 1 }

    puts 'the new chart is the one running'
    session.deliver(:acct1, 'payment.failed')
    check('the edited transition is in force') { session.overview.first[:active].include?(:dunning) }
    check('the assign ran on the carried data') { session.fetch(:acct1).data[:failures] == 2 }
    check('the new delayed send is pending') { session.overview.first[:sends] == 1 }
    session.advance(3_000)
    check('the delayed send fired') { session.overview.first[:active].include?(:past_due) }

    puts 'declarations added and removed'
    dir.join('actors.rb').write(<<~DECL)
      Workbench.actor :acct1, chart: :subscription
      Workbench.actor :acct2, chart: :subscription
    DECL
    session.reload!
    check('the added actor was spawned') { session.actors.key?(:acct2) }
    check('the actor already running was left alone') do
      session.overview.find { |actor| actor[:name] == :acct1 }[:active].include?(:past_due)
    end

    dir.join('actors.rb').write("Workbench.actor :acct2, chart: :subscription\n")
    session.reload!
    check('the removed actor is gone') { !session.actors.key?(:acct1) }
    check('its address was released') do
      session.context.find(session.context.address(:subscription, 'acct1')).first != 'ok'
    end

    puts 'a broken source file'
    errors_before = journal.entries.count { |entry| entry.level == :error }
    running_before = session.overview.first[:active]
    dir.join('subscription.rb').write("TinyactorsTest.chart :subscription do\n  this is not ruby\n")
    session.reload!
    check('the error is reported') do
      journal.entries.count { |entry| entry.level == :error } > errors_before
    end
    check('the actor was left running') do
      session.actors.key?(:acct2) && session.overview.first[:active] == running_before
    end
    check('the system kept running') do
      session.overview.first && session.overview.first[:residency] == 'idle'
    end
    session.deliver(:acct2, 'payment.failed')
    check('the actor still takes events') { session.overview.first[:active].include?(:dunning) }

    puts 'a fixed source file'
    dir.join('subscription.rb').write(SUBSCRIPTION_V2)
    session.reload!
    check('the actor is replaced once the chart loads again') do
      session.actors.key?(:acct2) && session.overview.first[:active].include?(:active)
    end

    puts 'a chart whose family changed'
    dir.join('subscription.rb').write(RENAMED)
    session.reload!
    check('the actor was respawned instead') do
      session.actors.key?(:acct2) && session.overview.first[:address] == 'billing/acct2'
    end
    check('the respawn was reported as such') do
      journal.entries.any? { |entry| entry.level == :warn && entry.text.include?('respawned') }
    end

    puts 'teardown'
    session.context.teardown!
    puts '  ok   the system released everything it allocated'
  rescue StandardError => error
    FAILURES << "#{error.class}: #{error.message}"
    puts "  FAIL #{error.class}: #{error.message}"
    puts error.backtrace.first(12).map { |line| "       #{line}" }
  ensure
    begin
      driver.close
    rescue StandardError => error
      puts "  note host shutdown: #{error.message}"
    end
  end
end

# An actor with invoked children. ta_actor_destroy does not cascade into the
# children an invoke started, so replacing the parent has to reap them by
# hand; the allocator check at teardown is what proves it did.
#
# supervisor.rb sorts before worker.rb but inlines it, so this also exercises
# the loader's several passes.
SUPERVISOR = <<~CHART
  TinyactorsTest.chart :supervisor do
    data ticks: 0
    initial :supervising
    state :supervising do
      invoke chart: :worker, id: :worker
      transition on: 'ping', type: :internal do
        assign '.ticks', '.ticks + 1'
      end
    end
  end
CHART

SUPERVISOR_V2 = SUPERVISOR.sub("data ticks: 0", "data ticks: 0\n  data revision: 2")

WORKER = <<~CHART
  TinyactorsTest.chart :worker do
    initial :working
    state :working do
      transition on: 'finish', target: :finished
    end
    final :finished
  end
CHART

Dir.mktmpdir('workbench-invoke') do |tmp|
  dir = Pathname.new(tmp)
  dir.join('supervisor.rb').write(SUPERVISOR)
  dir.join('worker.rb').write(WORKER)
  dir.join('actors.rb').write("Workbench.actor :boss, chart: :supervisor\n")

  journal = Workbench::Journal.new
  sources = Workbench::Sources.new(dir)
  sources.reload!
  driver = TinyactorsTest::Driver.new(HOST, timeout: 30)
  session = Workbench::Session.new(driver: driver, sources: sources, journal: journal)

  begin
    puts 'invoked children'
    check('both charts loaded despite the filename order') do
      sources.ok? && TinyactorsTest.charts.key?(:supervisor) && TinyactorsTest.charts.key?(:worker)
    end

    session.start!
    check('the supervisor is running') { session.actors.key?(:boss) }
    check('it has an invoked child') { session.overview.first[:children].size == 1 }

    child = session.overview.first[:children].first[:handle]
    session.deliver(:boss, 'ping')
    check('the child is a live actor') do
      session.context.ta.actor.inspect(system: session.context.system_handle, actor: child)[:result] == 'ok'
    end

    dir.join('supervisor.rb').write(SUPERVISOR_V2)
    session.reload!
    check('the supervisor was replaced') { session.actors.key?(:boss) }
    check('the old child was reaped') do
      session.context.ta.actor.inspect(system: session.context.system_handle,
                                       actor: child)[:result] == 'err_not_found'
    end
    check('the replacement started its own child') do
      children = session.overview.first[:children]
      children.size == 1 && children.first[:handle] != child
    end
    check('no actor was orphaned') do
      resident = session.context.rpc('system.actors', system: session.context.system_handle)
      # the supervisor and exactly one child
      resident.size == 2
    end

    puts 'teardown'
    session.context.teardown!
    puts '  ok   nothing leaked, so every child was accounted for'
  rescue StandardError => error
    FAILURES << "#{error.class}: #{error.message}"
    puts "  FAIL #{error.class}: #{error.message}"
    puts error.backtrace.first(10).map { |line| "       #{line}" }
  ensure
    begin
      driver.close
    rescue StandardError => error
      puts "  note host shutdown: #{error.message}"
    end
  end
end

puts
if FAILURES.empty?
  puts 'workbench self-test passed'
else
  puts "workbench self-test failed: #{FAILURES.size} check(s)"
  FAILURES.each { |failure| puts "  - #{failure}" }
  exit 1
end
