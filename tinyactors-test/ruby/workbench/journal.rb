# frozen_string_literal: true

module Workbench
  # The workbench's own record of what happened: reload outcomes, command
  # results, errors, and what the charts logged. It is what the logs pane
  # shows, kept bounded so a long session cannot grow without limit.
  class Journal
    Entry = Data.define(:at, :level, :text) do
      def to_s = text
    end

    LEVELS = %i[chart info warn error].freeze

    attr_reader :entries

    def initialize(limit: 500)
      @limit = limit
      @entries = []
      @generation = 0
    end

    # Bumped whenever something is recorded, so a renderer can tell whether
    # it needs to redraw without comparing the whole buffer.
    attr_reader :generation

    def add(level, text)
      raise ArgumentError, "unknown level #{level}" unless LEVELS.include?(level)

      text.to_s.each_line(chomp: true) do |line|
        @entries << Entry.new(at: Time.now, level: level, text: line)
      end
      @entries.shift(@entries.size - @limit) if @entries.size > @limit
      @generation += 1
      nil
    end

    def info(text)  = add(:info, text)
    def warn(text)  = add(:warn, text)
    def error(text) = add(:error, text)
    def chart(text) = add(:chart, text)

    # Records an exception as one entry per line, naming its class.
    def exception(prefix, error)
      add(:error, "#{prefix}: #{error.class.name.split('::').last}: #{error.message}")
    end

    def last(count)
      @entries.last(count)
    end
  end
end
