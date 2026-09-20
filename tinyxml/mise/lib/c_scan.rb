# frozen_string_literal: true

# A conservative scanner for the file-level shape of this project's C: function
# definitions, function prototypes and function-pointer declarators. It is not a
# C parser. It relies on the conventions in AGENTS.md (one declaration per
# statement, braces balanced, no K&R parameter lists) and errs by ignoring what
# it does not recognise, so the tasks that use it (lint-src, amalgamate) each
# check their result against something else: the compiler and the linker.
module CScan
  KEYWORDS = %w[if for while switch return sizeof _Alignof _Static_assert defined].freeze

  # A function found at file level. `start` is the offset of the first token of
  # its declaration (after any preprocessor lines), `name_at` the offset of its
  # name, `params` the text between the parentheses, and `body` the range of the
  # braces of a definition (nil for a prototype). Offsets are valid in the
  # original text as well as in the stripped copy.
  Function = Struct.new(:name, :start, :name_at, :params, :body, keyword_init: true) do
    def definition? = !body.nil?
  end

  # An ALL_CAPS name is an annotation or macro call, never a function here.
  MACRO_NAME = /\A[A-Z][A-Z0-9_]*\z/

  module_function

  # Blank out comments, strings and character literals, keeping every newline
  # and every other offset, so line numbers and matching braces stay meaningful.
  def strip(text)
    text.gsub(%r{/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'}m) { |match| match.gsub(/[^\n]/, ' ') }
  end

  def count_parameters(text)
    text = text.strip
    return 0 if text.empty? || text == 'void'

    depth = 0
    commas = 0
    text.each_char do |char|
      depth += 1 if '([{'.include?(char)
      depth -= 1 if ')]}'.include?(char)
      commas += 1 if char == ',' && depth.zero?
    end
    commas + 1
  end

  # The offset of the `(` that matches the `)` at `close`, or nil.
  def matching_open(text, close)
    depth = 0
    close.downto(0) do |i|
      depth += 1 if text[i] == ')'
      depth -= 1 if text[i] == '('
      return i if depth.zero?
    end
    nil
  end

  def line_of(text, index) = text[0, index].count("\n") + 1

  # The offset where the declaration in text[from...to] really starts: past
  # blank space and past preprocessor lines (with their continuations).
  def declaration_start(text, from, to)
    position = from
    continued = false
    while position < to
      line_end = text.index("\n", position) || to
      line = text[position...line_end]
      if continued || line.lstrip.start_with?('#')
        continued = line.rstrip.end_with?('\\')
        position = line_end + 1
      elsif line.strip.empty?
        position = line_end + 1
      else
        return position + (line.length - line.lstrip.length)
      end
    end
    to
  end

  # The functions of `text` (the original, not the stripped copy), in order.
  def functions(text)
    stripped = strip(text)
    found = []
    depth = 0
    statement_start = 0
    linkage_blocks = 0 # open `extern "C" {` blocks, which do not nest declarations

    stripped.each_char.with_index do |char, i|
      case char
      when '{'
        if depth.zero? && stripped[statement_start...i].match?(/\bextern\s*\z/)
          linkage_blocks += 1
          statement_start = i + 1
          next
        end
        if depth.zero?
          from = declaration_start(stripped, statement_start, i)
          before = stripped[from...i].rstrip
          if before.end_with?(')') && !before.match?(/\b(?:struct|union|enum)\b[^()]*\z/) && !before.include?('=')
            close = from + before.length - 1
            open = matching_open(stripped, close)
            head = open && stripped[from...open].rstrip
            if head && (name = head[/(\w+)\z/, 1]) && !KEYWORDS.include?(name) && !name.match?(MACRO_NAME)
              nesting = 0
              finish = (i...stripped.length).find do |k|
                nesting += 1 if stripped[k] == '{'
                nesting -= 1 if stripped[k] == '}'
                nesting.zero?
              end
              found << Function.new(name: name, start: from, name_at: from + head.length - name.length,
                                    params: stripped[open + 1...close], body: i..finish)
            end
          end
        end
        depth += 1
      when '}'
        if depth.zero? && linkage_blocks.positive?
          linkage_blocks -= 1
          statement_start = i + 1
          next
        end
        depth -= 1
        statement_start = i + 1 if depth.zero?
      when ';'
        next unless depth.zero?

        from = declaration_start(stripped, statement_start, i)
        # Trailing annotation macros, as in `f(x) TXML_LIFETIME(document);`.
        statement = stripped[from...i].sub(/(?:\s+[A-Z][A-Z0-9_]*\([^()]*\))+\s*\z/, '')
        if statement.include?(')') && !statement.include?('typedef') && !statement.match?(/\(\s*\*/) && !statement.include?('=')
          close = from + statement.rstrip.rindex(')')
          open = matching_open(stripped, close)
          head = open && stripped[from...open].rstrip
          if head && (name = head[/(\w+)\z/, 1]) && !KEYWORDS.include?(name) && !name.match?(MACRO_NAME)
            found << Function.new(name: name, start: from, name_at: from + head.length - name.length,
                                  params: stripped[open + 1...close], body: nil)
          end
        end
        statement_start = i + 1
      end
    end
    found
  end

  # Function-pointer declarators, `(*name)(params)`, anywhere in the text:
  # [[name, params, offset]].
  def function_pointers(text)
    stripped = strip(text)
    result = []
    stripped.scan(/\(\s*\*\s*(\w+)\s*\)\s*\(/) do
      open = Regexp.last_match.end(0) - 1
      depth = 0
      close = (open...stripped.length).find do |i|
        depth += 1 if stripped[i] == '('
        depth -= 1 if stripped[i] == ')'
        depth.zero?
      end
      result << [Regexp.last_match(1), stripped[open + 1...close], open]
    end
    result
  end
end
