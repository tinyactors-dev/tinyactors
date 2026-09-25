# frozen_string_literal: true

require 'json'

module Workbench
  # ANSI helpers. The panes are dashboards rather than scrolling output, so
  # each render clears and repaints; nothing depends on the terminal's
  # scrollback.
  module Ansi
    CLEAR = "\e[H\e[2J"
    HIDE  = "\e[?25l"
    RESET = "\e[0m"
    CODES = { dim: 2, bold: 1, red: 31, green: 32, yellow: 33, blue: 34, magenta: 35, cyan: 36, grey: 90 }.freeze

    module_function

    def paint(text, *styles)
      return text if styles.empty?

      "\e[#{styles.map { |style| CODES.fetch(style) }.join(';')}m#{text}#{RESET}"
    end

    # The printable width of a string, ignoring the escape sequences in it.
    def width(text) = text.gsub(/\e\[[0-9;?]*[a-zA-Z]/, '').length

    # Truncates to `limit` printable columns, keeping escape sequences intact.
    def clip(text, limit)
      return text if width(text) <= limit

      out = +''
      seen = 0
      text.scan(/\e\[[0-9;?]*[a-zA-Z]|./m) do |piece|
        if piece.start_with?("\e")
          out << piece
        else
          break if seen >= limit

          out << piece
          seen += 1
        end
      end
      "#{out}#{RESET}"
    end
  end

  # One tmux pane, written to through its tty. The pane itself runs a command
  # that produces no output, so nothing competes for the terminal.
  class Pane
    attr_reader :target

    def initialize(target)
      @target = target
      @tty = Tmux.pane_tty(target)
      @io = File.open(@tty, 'w')
      @io.sync = true
      @io.write(Ansi::HIDE)
      @last = nil
      @size = nil
      @sized_at = 0.0
    end

    # Asking tmux costs a subprocess, and a running clock redraws several
    # times a second, so the answer is cached briefly. A pane that is resized
    # corrects itself within SIZE_TTL.
    SIZE_TTL = 1.0

    def size
      now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      if @size.nil? || now - @sized_at > SIZE_TTL
        @size = Tmux.pane_size(@target)
        @sized_at = now
      end
      @size
    end

    # Repaints the pane with `lines`, which a block builds from the pane's
    # current width and height.
    def draw
      width, height = size
      lines = yield(width, height)
      frame = lines.first(height).map { |line| Ansi.clip(line, width) }.join("\r\n")
      return if frame == @last

      @last = frame
      @io.write("#{Ansi::CLEAR}#{frame}")
    rescue Errno::EIO, IOError
      nil
    end

    def close
      @io.close unless @io.closed?
    rescue IOError
      nil
    end
  end

  # Pretty-prints JSON as lines tagged with the path they render, so a
  # renderer can tell which lines carry a value that changed.
  module JSONView
    module_function

    # => [[path, text], ...]
    def lines(value, path = [], indent = 0, prefix = '', suffix = '', out = [])
      pad = '  ' * indent
      case value
      when Hash
        if value.empty?
          out << [path, "#{pad}#{prefix}{}#{suffix}"]
        else
          out << [nil, "#{pad}#{prefix}{"]
          keys = value.keys
          keys.each_with_index do |key, index|
            lines(value[key], path + [key.to_s], indent + 1,
                  "#{Ansi.paint(JSON.generate(key.to_s), :cyan)}: ",
                  index == keys.size - 1 ? '' : ',', out)
          end
          out << [nil, "#{pad}}#{suffix}"]
        end
      when Array
        if value.empty?
          out << [path, "#{pad}#{prefix}[]#{suffix}"]
        else
          out << [nil, "#{pad}#{prefix}["]
          value.each_with_index do |item, index|
            lines(item, path + [index.to_s], indent + 1, '', index == value.size - 1 ? '' : ',', out)
          end
          out << [nil, "#{pad}]#{suffix}"]
        end
      else
        out << [path, "#{pad}#{prefix}#{JSON.generate(value)}#{suffix}"]
      end
      out
    end

    # The leaf values of a document, keyed by their path.
    def leaves(value, path = [], out = {})
      case value
      when Hash then value.each { |key, inner| leaves(inner, path + [key.to_s], out) }
      when Array then value.each_with_index { |item, index| leaves(item, path + [index.to_s], out) }
      else out[path] = value
      end
      out
    end
  end
end
