# frozen_string_literal: true

# `<log>` and the host sink it goes to (ADR 0045). The claims are in
# include/tinyactors.h and docs/internal/public-api.md: the record carries
# the logging actor, the label and the evaluated value; writing it returns
# nothing, because SCXML 4.7 requires `<log>` to have no effect on document
# interpretation; and without a sink a `<log>` still evaluates its
# expression, so its errors do not depend on whether a host is listening.
module LogSink
  # Logs a label with a value, a label without one, and then logs again
  # from a transition, so the sink sees records from more than one block.
  def self.chart
    TinyactorsTest::Chart.define(:logging) do
      data phase: 'start'
      data worked: false

      state :ready do
        on_entry do
          log 'phase', expr: '.phase'
          log 'bare'
        end
        transition on: 'work', type: :internal do
          assign '.phase', '"working"'
          assign '.worked', 'true'
          log 'phase', expr: '.phase'
        end
      end
    end
  end

  # Logs an expression that fails, then records the error the chart sees.
  # The same shape as the `<log>` row of fault_contexts.rb, which runs
  # without a sink; here a sink is listening.
  def self.failing_chart
    TinyactorsTest::Chart.define(:logging_failure) do
      data outcome: nil

      state :ready do
        on_entry do
          log 'before', expr: '"ok"'
          log 'boom', expr: 'error("boom")'
          log 'after', expr: '"unreachable"'
        end
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end
  end
end

TinyactorsTest.scenarios 'log sink' do
  # Catches: a sink that is handed the label without the value (or the
  # other way round), that reports the wrong actor, or that misses the
  # records of a transition's block; a `<log>` without an expression that
  # is dropped instead of written with no value (both are optional, see
  # include/tinyactors.h).
  scenario 'a chart\'s log records reach the sink with label, value and actor' do
    recording_log
    actor = spawn(LogSink.chart)

    assert_logs %w[phase start], ['bare', nil]
    assert_equal [actor, actor], logs.map { |record| record[:actor] }

    actor.deliver 'work'
    assert_logs %w[phase start], ['bare', nil], %w[phase working]
    assert_logged 'phase', 'working', actor: actor
    assert_logged 'phase', 'start', times: 1
  end

  # SCXML 4.7: `<log>` must not affect interpretation, and public-api.md:
  # "Without a sink a `<log>` still evaluates its expression, so its errors
  # do not depend on whether a host is listening." The same two charts run
  # twice here, once with nobody listening and once with a sink installed.
  #
  # Catches: a `<log>` whose expression is only evaluated when a sink is
  # installed (the failing chart would then reach error.execution in one
  # half and not in the other); an installed sink that changes what the
  # actor's data or configuration becomes.
  scenario 'a sink changes nothing the chart does' do
    unheard = spawn(LogSink.chart)
    unheard.deliver 'work'
    unheard_failure = spawn(LogSink.failing_chart)

    recording_log
    heard = spawn(LogSink.chart)
    heard.deliver 'work'
    heard_failure = spawn(LogSink.failing_chart)

    assert_equal unheard.active, heard.active, 'the configuration is the same'
    assert_equal unheard.data, heard.data, 'the data is the same'
    assert_equal 'error.execution', unheard_failure.data('.outcome'),
                 'a failing <log> is an execution error with nobody listening'
    assert_equal unheard_failure.data, heard_failure.data,
                 'and exactly the same one while a sink listens'
    assert_logs %w[phase start], ['bare', nil], %w[phase working], %w[before ok]
  end

  # public-api.md: "Without a sink a `<log>` still evaluates its
  # expression, so its errors do not depend on whether a host is
  # listening." fault_contexts.rb pins the no-sink half; this pins that a
  # listening host sees the same error, and that the record of a failed
  # evaluation is not written.
  #
  # Catches: a sink that is handed a half-evaluated record; a failing
  # `<log>` that stops being error.execution once a host listens; a failed
  # log that skips the rest of the enclosing block differently from every
  # other action (SCXML 4.9).
  scenario 'a log expression that fails is error.execution while a sink listens' do
    recording_log
    actor = spawn(LogSink.failing_chart)

    assert_equal 'error.execution', actor.data('.outcome')
    assert_logs %w[before ok]
    assert_logged 'boom', nil, times: 0
    assert_logged 'after', 'unreachable', times: 0
  end
end
