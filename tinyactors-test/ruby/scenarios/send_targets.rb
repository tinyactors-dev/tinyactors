# frozen_string_literal: true

# One SCXML target grammar. A `<send>` target means the same thing however
# the author spells it: `target="..."` is parsed by core, while `targetexpr`
# is evaluated to a data-model value and decoded by the data model. Both
# reach the same `ta_scxml_target`, so both must produce the same outcome.
#
# Every expectation below comes from a mandatory W3C IRP assertion that this
# suite already runs:
#
# - test194 (6.2): an unsupported or invalid target is `error.execution`.
# - test496 (C.1): a session that does not exist or is inaccessible is
#   `error.communication`. Its own target is `#_scxml_foo`, so a session URI
#   whose text is not a session ID belongs to this rule, not to test194's.
#
# SCXML defines `#_internal`, `#_parent`, `#_<invokeid>`, and
# `#_scxml_<sessionid>`. `#_self` is not one of them, so it reads as the
# invocation named "self", which no chart here has started.
module SendTargets
  # [target text, the event the sending chart must see]
  TABLE = [
    ['#_internal', 'hit'],
    ['#_self', 'error.communication'],
    ['#_parent', 'error.communication'],
    ['#_scxml_99999999', 'error.communication'],
    ['#_scxml_4x', 'error.communication'],
    ['elsewhere', 'error.execution']
  ].freeze

  # A chart that sends `hit` to `target` on entry and records what comes
  # back: the event itself, or the error the send produced. `spelling` is
  # :literal for `target=`, :expr for `targetexpr=`, and :none for a chart
  # that only receives.
  def self.chart(target, spelling, datamodel = 'jq')
    jq = datamodel != 'ecmascript'
    outcome = jq ? '.outcome' : 'outcome'
    hit = jq ? '"hit"' : "'hit'"
    event_name = jq ? '$_event.name' : '_event.name'
    quoted = jq ? "\"#{target}\"" : "'#{target}'"
    TinyactorsTest::Chart.define(:probe, datamodel: datamodel) do
      jq ? data(outcome: nil) : data(:outcome)
      state :ready do
        on_entry do
          case spelling
          when :literal then send_event 'hit', to: target
          when :expr then send_event 'hit', to_expr: quoted
          end
        end
        transition on: 'hit', type: :internal do
          assign outcome, hit
        end
        transition on: 'error', type: :internal do
          assign outcome, event_name
        end
      end
    end
  end

  # {target => [outcome as a literal, outcome as an expression]}.
  def self.outcomes(context, datamodel)
    TABLE.to_h do |target, _|
      [target, %i[literal expr].map { |spelling| context.spawn(chart(target, spelling, datamodel)).data('.outcome') }]
    end
  end

  # The same table with the outcome each spelling must produce.
  def self.expected = TABLE.to_h { |target, outcome| [target, [outcome, outcome]] }
end

TinyactorsTest.scenarios 'send targets' do
  # Catches: a target grammar that lives in more than one place. Core parses
  # a literal target itself; a data model parses an evaluated one. Today
  # `#_self` is a delivery to one of them and an unreachable invocation to
  # the other, and a malformed session URI is error.execution to one and
  # error.communication to the other, which is what test496 requires.
  expect_failure 'the data model has its own target grammar, which disagrees with core',
                 message: /every target means the same written either way/
  scenario 'a target means the same as a literal and as an expression' do
    assert_equal SendTargets.expected, SendTargets.outcomes(self, 'jq'),
                 'every target means the same written either way'
  end

  # Catches: the same drift in a second data model, which would make the
  # meaning of a target depend on the chart's `datamodel`.
  expect_failure 'the ECMAScript data model has the same separate grammar',
                 message: /every target means the same written either way/
  scenario 'a target means the same in the ECMAScript data model' do
    assert_equal SendTargets.expected, SendTargets.outcomes(self, 'ecmascript'),
                 'every target means the same written either way'
  end

  # Catches: a session URI that stops resolving to its session. This row
  # agrees today, and keeps both spellings honest while the grammar moves.
  scenario 'a session URI delivers to that session, written either way' do
    %i[literal expr].each do |spelling|
      receiver = spawn(SendTargets.chart(nil, :none))
      sender = spawn(SendTargets.chart("#_scxml_#{receiver.handle}", spelling))
      assert_equal 'hit', receiver.data('.outcome'), "a #{spelling} session URI reaches its session"
      assert_equal nil, sender.data('.outcome'), "the sender saw no error (#{spelling})"
    end
  end
end
