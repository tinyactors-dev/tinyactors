# frozen_string_literal: true

# Where a chart's elements were written (core ADR 0086). The chart DSL
# records the file and line of every call as a span of the spec, the spec
# crosses the RPC with them, core keeps them per definition, and
# ta_definition_span answers by kind and ID. A debugger builds its line
# breakpoints and frame locations from this.
module SourceSpanCharts
  LINES = {}

  TinyactorsTest.chart :spans_order do
    LINES[:idle] = __LINE__ + 1
    state :idle do
      LINES[:go] = __LINE__ + 1
      transition on: 'go', target: :busy do
        LINES[:raise] = __LINE__ + 1
        raise_event 'started'
      end
    end
    LINES[:busy] = __LINE__ + 1
    state :busy do
      transition on: 'started', target: :done
    end
    final :done
  end

  # The same chart twice, under one name, at different lines of this file.
  FIRST = TinyactorsTest::Chart.define(:spans_moved) do
    state(:a) { transition on: 'go', target: :b }
    final :b
  end

  SECOND = TinyactorsTest::Chart.define(:spans_moved) do

    state(:a) { transition on: 'go', target: :b }
    final :b
  end
end

TinyactorsTest.scenarios 'source spans', chart: :spans_order do
  # Catches: a DSL chart whose elements lose where they were written on the
  # way through the RPC and the compiler.
  scenario 'every state, transition and action says the file and line it was written on' do
    lines = SourceSpanCharts::LINES
    idle = source_span(:state, state_id(definition, :idle))
    assert_equal [File.expand_path(__FILE__), lines[:idle]], [idle[:source], idle.dig(:span, :line)]
    assert_equal lines[:busy], source_span(:state, state_id(definition, :busy)).dig(:span, :line)
    assert_equal lines[:go], source_span(:transition, 0).dig(:span, :line)
    assert_equal lines[:raise], source_span(:action, 0).dig(:span, :line)
    # The DSL knows lines, not columns or bytes.
    assert_equal({ line: lines[:raise] }, source_span(:action, 0)[:span].slice(:line, :column, :length).compact
                                                                      .reject { |_, value| value.zero? })
  end

  # Catches: a span that is not an element's, or one beyond the counts.
  scenario 'an element without a span, and an ID beyond the counts, have none' do
    facts = definition_facts
    # The document's initial transition was never written: it is SCXML's default.
    assert_equal nil, source_span(:transition, facts[:transition_count] - 1)
    assert_equal nil, source_span(:state, facts[:state_count])
    assert_equal nil, source_span(:expression, 0)
  end

  # Catches: a revision that moves with the chart's text, which would keep
  # every session image of a reformatted chart from restoring (ADR 0038);
  # and a definition that keeps the old lines when the moved chart is
  # registered again and answered by it (core ADRs 0077 and 0086).
  scenario 'a chart moved down a line keeps its revision and its definition, which says where it is now' do
    first = definition(SourceSpanCharts::FIRST)
    line = source_span(:state, 0, first).dig(:span, :line)
    second = definition(SourceSpanCharts::SECOND)
    assert_equal first, second
    assert_equal definition_facts(first)[:identity], definition_facts(second)[:identity]
    # SECOND's state is written six lines below FIRST's.
    assert_equal line + 6, source_span(:state, 0, first).dig(:span, :line)
  end

  # Catches: an export that drops where elements were written (ADR 0042).
  scenario 'the export carries the spans back' do
    exported = export
    assert_equal [{ name: File.expand_path(__FILE__) }], exported[:sources]
    idle = exported[:states].find { |state| state[:id] == 'idle' }
    assert_equal SourceSpanCharts::LINES[:idle], exported[:spans][idle[:span] - 1][:line]
  end
end
