# frozen_string_literal: true

require 'pathname'

# The interactive test workbench: a long-lived host process whose charts are
# read from a directory on disk and reloaded when they change. See
# ruby/workbench/README.md.
module Workbench
  class SourceError < StandardError; end

  # One top-level actor the directory declares. Every top-level actor is a
  # named actor, so a chart edit can replace it through the core replacement
  # protocol instead of restarting the system.
  ActorSpec = Data.define(:name, :chart, :binding)

  class << self
    # The Sources currently loading, so `Workbench.actor` in a source file
    # knows where to record its declaration.
    attr_accessor :collector

    # Declares a named actor: `Workbench.actor :acct1, chart: :subscription`.
    def actor(name, chart:, binding: nil)
      unless collector
        raise SourceError, 'Workbench.actor is only callable from a workbench source file'
      end

      collector.declare(name, chart, binding)
    end
  end

  # The charts and actor declarations of one directory.
  #
  # Every *.rb file in the directory is loaded for its side effects: it may
  # define charts (TinyactorsTest.chart) and declare actors
  # (Workbench.actor). Files are loaded in several passes, because a chart
  # that inlines another with `invoke chart:` resolves it at definition time
  # and so must be loaded after it; the pass order sorts that out without the
  # author naming files to suit it.
  #
  # Charts live in TinyactorsTest.charts, the same Hash the Context reads, so
  # a reload that replaces a Chart object is visible to the running system.
  class Sources
    attr_reader :dir, :actors, :errors, :specs

    def initialize(dir)
      @dir = Pathname.new(dir).expand_path
      @actors = {}
      @errors = {}
      # chart name => the spec it last loaded with, to tell a real edit from
      # a touched file.
      @specs = {}
      # chart name => the file that defined it.
      @chart_files = {}
    end

    # The file a chart was defined in, or nil.
    def file_of(chart) = @chart_files[chart.to_sym]

    def files
      Dir[@dir.join('*.rb').to_s].sort
    end

    def charts
      TinyactorsTest.charts
    end

    def ok?
      @errors.empty?
    end

    # Records a declaration from `Workbench.actor`.
    def declare(name, chart, binding)
      name = name.to_sym
      if @actors.key?(name)
        raise SourceError, "actor #{name} is declared twice"
      end

      @actors[name] = ActorSpec.new(name: name, chart: chart.to_sym, binding: binding)
    end

    # Reloads every file in the directory. Returns the chart names whose spec
    # changed since the last load. Files that fail are recorded in `errors`
    # and leave nothing behind, so a syntax error in one file cannot half
    # define its charts.
    def reload!
      previous = @specs
      charts.clear
      @actors = {}
      @errors = {}
      @chart_files = {}
      Workbench.collector = self
      begin
        load_files
      ensure
        Workbench.collector = nil
      end
      @specs = charts.transform_values(&:to_spec)
      (previous.keys | @specs.keys).select { |name| previous[name] != @specs[name] }
    end

    private

    # Loads the pending files repeatedly while any of them makes progress, so
    # a chart that depends on another is loaded after it whatever the
    # filenames are. A file that still fails once no pass makes progress is
    # reported with the error of its last attempt.
    def load_files
      pending = files
      until pending.empty?
        failed = []
        progressed = false
        pending.each do |file|
          if load_file(file)
            progressed = true
          else
            failed << file
          end
        end
        break unless progressed

        pending = failed
      end
    end

    # Loads one file. On failure, removes whatever it defined before it
    # raised, so the next pass can load it from a clean slate.
    def load_file(file)
      charts_before = charts.keys
      actors_before = @actors.keys
      begin
        load file
        @errors.delete(file)
        (charts.keys - charts_before).each { |name| @chart_files[name] = file }
        true
      rescue ScriptError, StandardError => error
        (charts.keys - charts_before).each { |name| charts.delete(name) }
        (@actors.keys - actors_before).each { |name| @actors.delete(name) }
        @errors[file] = error
        false
      end
    end
  end
end
