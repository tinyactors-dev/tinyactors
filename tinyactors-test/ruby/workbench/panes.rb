# frozen_string_literal: true

module Workbench
  # The three display panes. Each turns the session into lines; the Pane it
  # is handed does the clipping and the writing.
  module Panes
    RESIDENCY = { 'idle' => :green, 'running' => :yellow, 'done' => :grey, 'unloaded' => :grey }.freeze

    # Every named actor, its address and definition revision, what it is
    # doing, and the invoked children hanging off it.
    class Overview
      def initialize(session) = @session = session

      def lines(width, _height)
        out = [header(width)]
        overview = @session.overview
        if overview.empty?
          out << ''
          out << Ansi.paint('  no actors declared', :grey)
          out << Ansi.paint("  add one to #{@session.sources.dir.basename}/:", :grey)
          out << Ansi.paint('    Workbench.actor :name, chart: :your_chart', :grey)
          return out
        end

        overview.each do |actor|
          out << ''
          out.concat(actor_lines(actor))
        end
        out
      end

      private

      def header(width)
        deadline = @session.next_deadline
        left = Ansi.paint(' actors ', :bold)
        clock = @session.clock&.describe
        left += Ansi.paint(" #{clock} ", :yellow) if clock
        right = +"t=#{@session.now}ms"
        right << "  next deadline #{deadline}ms" if deadline
        pad = [width - Ansi.width(left) - right.length - 1, 1].max
        "#{left}#{' ' * pad}#{Ansi.paint(right, :grey)}"
      end

      def actor_lines(actor)
        marks = []
        marks << Ansi.paint('RUNNING', :yellow) if actor[:running]
        marks << Ansi.paint('UNPUBLISHED', :red) unless actor[:published]
        marks << Ansi.paint("#{actor[:internal]} queued", :magenta) if actor[:internal].to_i.positive?
        marks << Ansi.paint("#{actor[:sends]} send(s)", :blue) if actor[:sends].to_i.positive?

        out = ["  #{Ansi.paint(actor[:name].to_s, :bold)} " \
               "#{Ansi.paint(actor[:address], :cyan)} " \
               "#{Ansi.paint("[#{actor[:residency]}]", RESIDENCY.fetch(actor[:residency], :grey))}" \
               "#{marks.empty? ? '' : " #{marks.join(' ')}"}"]
        out << Ansi.paint("    #{actor[:chart]}  def #{actor[:definition]} rev #{actor[:revision]}", :grey)
        out << "    #{Ansi.paint(active_text(actor[:active]), :green)}"
        actor[:children].each do |child|
          out << Ansi.paint("      +- invoked #{child[:family] || child[:handle]} " \
                            "[#{child[:residency]}] #{child[:active].join(' ')}", :grey)
        end
        out
      end

      def active_text(active)
        active.empty? ? '(no configuration)' : active.join(' ')
      end
    end

    # The focused actor's data-model root, with values that changed since the
    # last render highlighted.
    class DataModel
      def initialize(session)
        @session = session
        @previous = {}
        @focus = nil
      end

      def lines(width, _height)
        focus = @session.focus
        if focus.nil?
          return [Ansi.paint(' data model ', :bold), '', Ansi.paint('  no actor in focus', :grey)]
        end

        # A different actor is a different document; nothing is "changed".
        @previous = {} unless focus == @focus
        @focus = focus

        root = @session.focused_data
        out = [header(focus, width)]
        if root.nil?
          out << ''
          out << Ansi.paint('  this actor has no data-model root', :grey)
          return out
        end

        leaves = JsonView.leaves(root)
        changed = leaves.reject { |path, value| @previous.key?(path) && @previous[path] == value }.keys
        gone = @previous.keys - leaves.keys
        @previous = leaves

        out << ''
        JsonView.lines(root).each do |path, text|
          marker = path && changed.include?(path) ? Ansi.paint('>', :yellow) : ' '
          out << "#{marker} #{text}"
        end
        unless gone.empty?
          out << ''
          out << Ansi.paint("  removed: #{gone.map { |path| path.join('.') }.join(', ')}", :red)
        end
        out
      end

      private

      def header(focus, width)
        left = Ansi.paint(' data model ', :bold)
        right = "focus: #{focus}"
        pad = [width - Ansi.width(left) - right.length - 1, 1].max
        "#{left}#{' ' * pad}#{Ansi.paint(right, :grey)}"
      end
    end

    # What the workbench and the charts have to say: reloads, commands,
    # errors, and <log>.
    class Logs
      COLOURS = { chart: :cyan, info: :green, warn: :yellow, error: :red }.freeze
      TAGS    = { chart: 'log ', info: '--  ', warn: 'warn', error: 'ERR ' }.freeze

      def initialize(journal) = @journal = journal

      def lines(width, height)
        out = [Ansi.paint(' logs ', :bold) + Ansi.paint(" #{sources_note}", :grey)]
        body = height - out.size - 1
        entries = @journal.last([body, 1].max)
        entries.each do |entry|
          colour = COLOURS.fetch(entry.level)
          out << "#{Ansi.paint(entry.at.strftime('%H:%M:%S'), :grey)} " \
                 "#{Ansi.paint(TAGS.fetch(entry.level), colour)} #{Ansi.clip(entry.text, [width - 15, 10].max)}"
        end
        out
      end

      private

      def sources_note = ''
    end
  end
end
