# frozen_string_literal: true

module SCXML
  module IRP
    # Writes indented Ruby source.
    class Emitter
      def initialize
        @lines = []
        @depth = 0
      end

      def line(text = '')
        @lines << (text.empty? ? '' : ('  ' * @depth) + text)
      end

      def block(header)
        line("#{header} do")
        @depth += 1
        yield
        @depth -= 1
        line('end')
      end

      def to_s = "#{@lines.join("\n")}\n"
    end

    # Ruby literal helpers for generated source.
    module Ruby
      module_function

      def str(text)
        text = text.to_s
        return "'#{text}'" unless text.match?(/['\\\n]/)

        text.inspect
      end

      def sym(text)
        text = text.to_s
        text.match?(/\A[A-Za-z_]\w*\z/) ? ":#{text}" : ":#{text.inspect}"
      end
    end
  end
end
