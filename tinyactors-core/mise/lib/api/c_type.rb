# frozen_string_literal: true

module Api
  # Parses C type spellings into API type references with the C declarator
  # grammar. Two inputs use it: clang's printed types (`qualType`, abstract
  # declarators without names) and declarations read from the header's source
  # text (callback parameters, which carry names and annotation macros).
  # Anything outside the subset the public header uses is an error: arrays,
  # `volatile`, `restrict`, and unknown type names never pass silently.
  #
  # Type references:
  #   {"builtin" => "uint32_t"}              C builtin or <stdint.h>/<stddef.h> type
  #   {"type" => "ta_str"}                   a named API type
  #   {"pointer" => T}                       pointer to T
  #   {"function" => {"returns" => T, "params" => [param]}}
  # Any of them may carry "const" => true, qualifying that node itself.
  module CType
    Error = Class.new(StandardError)

    BUILTINS = %w[void char bool int int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t].freeze
    SPELLINGS = { '_Bool' => 'bool' }.freeze
    UNSUPPORTED = %w[volatile restrict _Atomic unsigned signed short long float double].freeze
    TOKEN = %r{\s+|/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[A-Za-z_]\w*|\d+|\.\.\.|[*()\[\],.]}m

    Declaration = Struct.new(:name, :type, :annotations, keyword_init: true)
    Annotation = Struct.new(:name, :args, keyword_init: true)

    module_function

    def tokenize(text)
      tokens = []
      position = 0
      while position < text.size
        match = TOKEN.match(text, position)
        raise Error, "unexpected #{text[position].inspect} in #{text.inspect}" unless match&.begin(0) == position

        token = match[0]
        tokens << token unless token.match?(%r{\A(\s|/[*/])})
        position = match.end(0)
      end
      tokens
    end

    # A type spelling without a name, e.g. "const ta_scxml_spec **".
    # `known` answers whether a name is a declared API type.
    def parse_type(spelling, known:)
      parser = Parser.new(tokenize(spelling), known: known, annotations: nil)
      declaration = parser.declaration(names: :forbidden)
      parser.finish
      declaration.type
    end

    # A declaration in source text, e.g. a callback field
    # "ta_result (*clone)(void *self, TA_OUT ta_value *out)". `annotations`
    # lists the macro names that may appear as annotations.
    def parse_declaration(text, known:, annotations:)
      parser = Parser.new(tokenize(text), known: known, annotations: annotations)
      declaration = parser.declaration(names: :required)
      parser.finish
      declaration
    end

    # One annotation macro invocation at the start of `text`, e.g.
    # TA_BUFFER(capacity, needed) or TA_SYMBOLS("TA_RUN_").
    def parse_annotation(text)
      name = text[/\A[A-Za-z_]\w*/] or raise Error, "expected an annotation name in #{text[0, 40].inspect}"
      rest = text[name.size..]
      open = rest[/\A\s*\(/]
      return Annotation.new(name: name, args: []) unless open

      depth = 0
      close = nil
      in_string = false
      rest.each_char.with_index do |char, index|
        next if index < open.size - 1

        if in_string
          in_string = false if char == '"' && rest[index - 1] != '\\'
        elsif char == '"'
          in_string = true
        elsif char == '('
          depth += 1
        elsif char == ')'
          depth -= 1
          if depth.zero?
            close = index
            break
          end
        end
      end
      raise Error, "unterminated arguments of #{name}" unless close

      Annotation.new(name: name, args: Parser.annotation_args(tokenize(rest[(open.size - 1)..close])))
    end

    # Renders a type reference as C: `render(T, "name")` is a declaration of
    # `name` with type T, `render(T)` an abstract type name.
    def render(type, name = '')
      render_declarator(type, name.to_s).strip
    end

    def render_declarator(type, inner)
      if type.key?('pointer')
        target = type['pointer']
        text = "*#{type['const'] ? 'const ' : ''}#{inner}"
        text = "(#{text})" if target.key?('function')
        render_declarator(target, text)
      elsif type.key?('function')
        function = type['function']
        params = function['params'].map { |param| render(param['type'], param['name']) }
        params = ['void'] if params.empty?
        returns = function['returns']
        # In a finished description, `returns` describes the result: {"type" => T, ...}.
        returns = returns['type'] if returns['type'].is_a?(Hash)
        render_declarator(returns, "#{inner}(#{params.join(', ')})")
      else
        base = type['builtin'] || type['type'] || raise(Error, "cannot render #{type.inspect}")
        "#{type['const'] ? 'const ' : ''}#{base} #{inner}"
      end
    end

    # Recursive-descent parser over tokens.
    class Parser
      def initialize(tokens, known:, annotations:)
        @tokens = tokens
        @known = known
        @annotations = annotations
      end

      def self.annotation_args(tokens)
        expect = ->(token) { raise Error, "expected #{token}" unless tokens.shift == token }
        expect.call('(')
        args = [[]]
        depth = 0
        loop do
          token = tokens.shift or raise(Error, 'unterminated annotation arguments')
          if token == ')' && depth.zero?
            break
          elsif token == ',' && depth.zero?
            args << []
          else
            depth += 1 if token == '('
            depth -= 1 if token == ')'
            args.last << token
          end
        end
        args.map { |parts| unquote(parts.join) }.reject(&:empty?)
      end

      def self.unquote(text)
        text.start_with?('"') ? text[1...-1] : text
      end

      def finish
        raise Error, "unexpected #{@tokens.first.inspect}" unless @tokens.empty?
      end

      # specifiers declarator, with annotation macros anywhere among the
      # specifiers. names: :forbidden, :optional, or :required.
      def declaration(names:)
        annotations = []
        const = false
        base = nil
        loop do
          token = @tokens.first
          if annotation?(token)
            annotations << annotation
          elsif token == 'const'
            @tokens.shift
            const = true
          elsif UNSUPPORTED.include?(token)
            raise Error, "unsupported type keyword #{token}"
          elsif %w[struct enum union].include?(token)
            raise Error, "duplicate type in declaration" if base

            @tokens.shift
            base = named(@tokens.shift)
          elsif base.nil? && identifier?(token)
            @tokens.shift
            base = named(token)
          else
            break
          end
        end
        raise Error, "missing type near #{@tokens.first.inspect}" unless base

        base = base.merge('const' => true) if const
        name, wrap = declarator(names)
        raise Error, 'missing declaration name' if names == :required && name.nil?
        raise Error, "unexpected name #{name}" if names == :forbidden && name

        Declaration.new(name: name, type: wrap.call(base), annotations: annotations)
      end

      private

      def annotation?(token)
        @annotations&.include?(token)
      end

      def annotation
        name = @tokens.shift
        args = @tokens.first == '(' ? self.class.annotation_args(@tokens) : []
        Annotation.new(name: name, args: args)
      end

      def identifier?(token) = token&.match?(/\A[A-Za-z_]\w*\z/)

      def named(token)
        raise Error, "expected a type name, got #{token.inspect}" unless identifier?(token)

        spelling = SPELLINGS.fetch(token, token)
        return { 'builtin' => spelling } if BUILTINS.include?(spelling)
        return { 'type' => spelling } if @known.call(spelling)

        raise Error, "unknown type #{spelling}"
      end

      # Returns [name, wrap]: wrap turns the base type into the declared type.
      def declarator(names)
        pointers = []
        while @tokens.first == '*'
          @tokens.shift
          const = false
          while %w[const].include?(@tokens.first) || UNSUPPORTED.include?(@tokens.first)
            raise Error, "unsupported pointer qualifier #{@tokens.first}" unless @tokens.first == 'const'

            @tokens.shift
            const = true
          end
          pointers << const
        end
        name, inner = direct_declarator(names)
        wrap = lambda do |base|
          type = pointers.reduce(base) { |target, const| const ? { 'pointer' => target, 'const' => true } : { 'pointer' => target } }
          inner.call(type)
        end
        [name, wrap]
      end

      def direct_declarator(names)
        name = nil
        inner = ->(type) { type }
        if @tokens.first == '(' && @tokens[1] == '*'
          @tokens.shift
          name, inner = declarator(names)
          raise Error, 'expected )' unless @tokens.shift == ')'
        elsif identifier?(@tokens.first) && !annotation?(@tokens.first)
          name = @tokens.shift
        end
        suffix = nil
        if @tokens.first == '('
          suffix = parameters
        elsif @tokens.first == '['
          raise Error, 'array declarators are not supported'
        end
        raise Error, 'only one declarator suffix is supported' if @tokens.first == '(' || @tokens.first == '['

        wrap = lambda do |type|
          type = { 'function' => { 'returns' => type, 'params' => suffix } } if suffix
          inner.call(type)
        end
        [name, wrap]
      end

      def parameters
        @tokens.shift
        if @tokens.first == 'void' && @tokens[1] == ')'
          @tokens.shift(2)
          return []
        end
        params = []
        loop do
          raise Error, 'variadic functions are not supported' if @tokens.first == '...'

          declaration = self.declaration(names: @annotations ? :required : :forbidden)
          params << { 'name' => declaration.name, 'type' => declaration.type, 'annotations' => declaration.annotations }
          token = @tokens.shift
          break if token == ')'
          raise Error, "expected , or ) in parameter list, got #{token.inspect}" unless token == ','
        end
        params
      end
    end
  end
end
