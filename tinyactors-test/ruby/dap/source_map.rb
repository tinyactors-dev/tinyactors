# frozen_string_literal: true

module DAP
  # Lines of the chart's Ruby files, and the compiled elements written on
  # them, from the definition's source spans (core ADR 0086). The Ruby DSL
  # records the file and line of each call that wrote a state, a
  # transition, an initial transition or an action; expressions are
  # arguments of those calls and have no span of their own.
  #
  # An index is built by walking every ID up to the counts inspection
  # reports, and kept until `forget`: a chart registered again after it
  # moved keeps its definition (core ADR 0077), whose spans then say where
  # it is written now (core ADR 0086).
  class SourceMap
    # One element and where it was written.
    Element = Data.define(:kind, :id, :path, :line)

    # When several elements are on one line, a breakpoint there means the
    # most specific: an action, then a transition, then a state.
    PREFERENCE = { action: 0, transition: 1, state: 2 }.freeze

    def initialize(labels)
      @labels = labels
      @elements = {}
    end

    # Drops every index, after a reload may have moved the spans.
    def forget = @elements.clear

    # Every element of the definition that has a span, in ID order by kind.
    def elements(definition)
      @elements[definition] ||= PREFERENCE.keys.flat_map do |kind|
        (0...@labels.count(definition, kind)).filter_map do |id|
          where = @labels.span(definition, kind, id)
          where && Element.new(kind: kind, id: id, path: where[:source], line: where[:line])
        end
      end
    end

    # The element a breakpoint on `line` of `path` stands for: one written
    # on that line, the most specific first; otherwise the nearest line
    # with an element, a following one on a tie. nil when the definition
    # has nothing in that file.
    def resolve(definition, path, line)
      candidates = elements(definition).select { |element| element.path == path }
      return nil if candidates.empty?

      nearest = candidates.map(&:line).uniq.min_by { |at| [(at - line).abs, at < line ? 1 : 0] }
      candidates.select { |element| element.line == nearest }.min_by { |element| [PREFERENCE[element.kind], element.id] }
    end

    # The lines of `path` between `from` and `to` that hold an element.
    def lines(definition, path, from, to)
      elements(definition).select { |element| element.path == path && element.line.between?(from, to) }
                          .map(&:line).uniq.sort
    end

    # The states at `line` of `path`, for gotoTargets: the states written
    # there, else the state an action or a transition written there belongs
    # to. Empty when nothing of this chart is on that line.
    def states_at(definition, path, line)
      here = elements(definition).select { |element| element.path == path && element.line == line }
      states = here.select { |element| element.kind == :state }.map(&:id)
      return states unless states.empty?

      here.filter_map { |element| owner_state(definition, element) }.uniq
    end

    # Where a frame is: {source:, line:, column:, end_line:, end_column:},
    # or nil.
    def location(definition, kind, id) = @labels.span(definition, kind, id)

    private

    def owner_state(definition, element)
      case element.kind
      when :transition
        info = @labels.transition_info(definition, element.id)
        info && !Labels.none?(info[:source]) ? Integer(info[:source]) : nil
      when :action
        outer = @labels.enclosing(definition, element.id).last || element.id
        info = @labels.action_info(definition, outer)
        return nil if info.nil?

        case info[:block]
        when 'onentry', 'onexit', 'finalize' then Integer(info[:owner])
        when 'transition' then owner_state(definition, Element.new(kind: :transition, id: Integer(info[:owner]),
                                                                    path: element.path, line: element.line))
        end
      end
    end
  end
end
