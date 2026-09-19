# frozen_string_literal: true

# Event names are structural, not strings (ADR 0007, invariant 16): a name
# is a path of interned segments, and matching compares ancestry and depth
# rather than text. `ta_event_name_matches` is that comparison, and a
# transition's `event` descriptor is the same rule applied to a chart.
#
# The rule is SCXML 3.12.1: a descriptor matches an event when it is the
# event's name or one of its ancestors, compared segment by segment. "err"
# is not a prefix of "error" in that sense, however much the text looks
# like one.
#
# Core answers that question twice, and nothing makes the two agree:
# `ta_event_name_matches` walks the interned trie by ancestry, while a
# transition's descriptor is a compiled pattern matched segment by segment
# (`ta_event_pattern_matches`), which also carries the wildcards the trie
# walk cannot express. Both scenarios below run the same table, so a change
# to either implementation alone is caught: breaking the trie walk fails
# the first, breaking the pattern matcher fails the second.
module EventNames
  # [event, descriptor, matches?]
  TABLE = [
    ['error', 'error', true],
    ['error.execution', 'error', true],
    ['error.communication.invoke.svc.unavailable', 'error', true],
    ['error.communication.invoke.svc.unavailable', 'error.communication', true],
    ['done.state.work', 'done.state', true],
    ['done.state.work', 'done', true],
    ['done.invoke.svc', 'done.invoke.svc', true],
    # A descriptor is never matched by something shorter than it.
    ['error', 'error.execution', false],
    ['done.state', 'done.state.work', false],
    # Segment boundaries: text prefixes are not name prefixes.
    ['error', 'err', false],
    ['error.execution', 'error.exec', false],
    ['errors.execution', 'error', false],
    # Different names of the same depth never match.
    ['error.execution', 'error.communication', false],
    ['done.state.work', 'done.state.other', false]
  ].freeze

  # A chart whose only transition listens for `descriptor` and records what
  # arrived, so the chart-level rule can be compared with the API's.
  def self.chart(descriptor)
    TinyactorsTest::Chart.define(:listener) do
      data seen: nil
      state :waiting do
        transition on: descriptor, type: :internal do
          assign '.seen', '$_event.name'
        end
      end
    end
  end
end

TinyactorsTest.scenarios 'event names' do
  # Catches: matching that compares formatted text rather than structure,
  # which would make "err" match "error" and "error.exec" match
  # "error.execution"; and matching that ignores depth, which would let a
  # descriptor be matched by a shorter name.
  scenario 'a descriptor matches a name by segments, not by text' do
    expected = EventNames::TABLE.to_h { |event, descriptor, matches| [[event, descriptor], matches] }
    actual = EventNames::TABLE.to_h do |event, descriptor, _|
      [[event, descriptor], event_matches?(event, descriptor)]
    end
    assert_equal expected, actual
  end

  # Catches: a chart whose transition descriptors follow a different rule
  # from `ta_event_name_matches`. SCXML 3.12.1 is one rule; a host that
  # reads a descriptor with the API must get the interpreter's answer.
  scenario 'a transition descriptor selects the same names as the API' do
    # A fresh listener per row, so each answer is about that row alone.
    EventNames::TABLE.each do |event, descriptor, matches|
      listener = spawn(EventNames.chart(descriptor))
      listener.deliver event
      assert_equal (matches ? event : nil), listener.data('.seen'),
                   "descriptor #{descriptor.inspect} against #{event.inspect}"
    end
  end

  # Catches: a root descriptor that stops being the wildcard SCXML requires
  # (3.12.1: "*" matches every event), or an event name that is not rooted
  # in the same tree as the descriptors compared against it.
  scenario 'every name matches the root descriptor' do
    catcher = TinyactorsTest::Chart.define(:catcher) do
      data seen: { expr: '[]' }
      state :waiting do
        transition on: '*', type: :internal do
          assign '.seen', '.seen + [$_event.name]'
        end
      end
    end
    listener = spawn(catcher)
    %w[go error.execution done.state.work a.b.c.d.e].each { |event| listener.deliver event }
    assert_equal %w[go error.execution done.state.work a.b.c.d.e], listener.data('.seen')
  end

  # Catches: names interned twice getting different IDs, which would break
  # matching and the trie that makes it cheap (ADR 0007). The same text is
  # the same name, whichever segments it was built from.
  scenario 'interning the same name twice gives the same name' do
    first = event_name('error.communication.invoke')
    second = event_name('error.communication.invoke')
    assert_equal first, second
    assert_equal 'error.communication.invoke', event_text(first)
    assert event_matches?('error.communication.invoke', 'error.communication'),
           'the interned name matches its own ancestor'
  end
end
