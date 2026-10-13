# frozen_string_literal: true

# The shapes generator (docs/matcher.md, section 4). It reads
# src/shapes.rules and the event and operator names of include/staticjs.h,
# checks the rules, normalises them as iburg does (every nested pattern a
# nonterminal of its own, identical ones merged), and writes the matcher's
# tables: src/shapes.h for the rest of src/ and the tests, and
# src/shapes_table.h for src/matcher.c alone.
#
#   StaticJS::Shapes.generate(rules, header) # => { 'src/shapes.h' => text, 'src/shapes_table.h' => text }
#
# A rules file that breaks a check raises StaticJS::Shapes::Error, whose
# message begins `shapes.rules:LINE:`.
module StaticJS
  module Shapes
    class Error < StandardError; end

    # Where the generated files go, relative to staticjs/.
    HEADER = 'src/shapes.h'
    TABLE = 'src/shapes_table.h'

    FIELD_TYPES = %w[count operator number text name read operand].freeze
    PARAM_TYPES = %w[text flag count number operator operand].freeze
    KEYWORDS = %w[event shape rewrite nonterminal leaf postfix open inner close item cost true false].freeze # MUTATION: rewrite
    # A captured text longer than this fails its test (docs/matcher.md, 5.2).
    TEXT_MAX = 64
    # The table's types: a cell's nonterminals in a uint64_t mask (0 is
    # none), variables in a uint8_t (0xFF is SHAPE_CONSTANT), bracket rules
    # of one open kind in a uint32_t mask, costs below SHAPE_INFINITE.
    MAX_NONTERMINALS = 64
    MAX_BRACKET_RULES = 32
    MAX_VARIABLES = 32
    MAX_COST = 0xFFFE
    # MUTATION: a rewrite's rules (docs/matcher.md, section 12): their
    # nonterminals, rewrites included, in a uint32_t mask (0 is none), and
    # bracket rules of one open kind in a uint32_t mask.
    MAX_ANCHORED_NONTERMINALS = 32

    module_function

    # Generate both files from the rules' text and the public header's text.
    def generate(rules, header, file: 'shapes.rules')
      kinds, operators = read_header(header)
      ast = Parser.new(Lexer.new(rules, file).tokens, file).parse
      model = Checker.new(ast, kinds, operators, file).check
      table = Normaliser.new(model).run
      { HEADER => Emitter.shapes_h(model), TABLE => Emitter.table_h(model, table) }
    end

    # The names of sjs_event_kind and sjs_operator, in order, from 1.
    def read_header(text)
      [enum_names(text, 'sjs_event_kind', 'SJS_EV_'), enum_names(text, 'sjs_operator', 'SJS_OP_')]
    end

    def enum_names(text, type, prefix)
      text = text.gsub(%r{/\*.*?\*/}m, '')
      body = text[/typedef enum \{([^}]*)\}\s*#{type};/m, 1] or raise Error, "staticjs.h: no #{type}"
      names = body.split(',').map(&:strip).reject(&:empty?)
      names.each_with_index.map do |entry, i|
        name, value = entry.split('=').map(&:strip)
        raise Error, "staticjs.h: #{type} has #{entry.inspect}" unless name.start_with?(prefix)
        raise Error, "staticjs.h: #{type} does not count from 1" if (i.zero? && value != '1') || (i.positive? && value)

        name.delete_prefix(prefix)
      end
    end

    # ------------------------------------------------------------------------
    # Lexer: words, numbers, strings and punctuation, with their lines.

    Token = Struct.new(:type, :value, :line)

    class Lexer
      PUNCTUATION = ['...', '<-', '(', ')', '{', '}', ',', ':', ';'].freeze # MUTATION: ...

      def initialize(text, file)
        @text = text
        @file = file
      end

      def tokens
        out = []
        line = 1
        i = 0
        while i < @text.length
          c = @text[i]
          if c == "\n"
            line += 1
            i += 1
          elsif c.match?(/\s/)
            i += 1
          elsif c == '#'
            i += 1 while i < @text.length && @text[i] != "\n"
          elsif (word = @text[i..][/\A[A-Za-z_][A-Za-z0-9_]*/])
            type = word.match?(/\A[A-Z][A-Z0-9_]*\z/) ? :upper : :lower
            out << Token.new(type, word, line)
            i += word.length
          elsif (number = @text[i..][/\A\d+(\.\d+)?/])
            out << Token.new(number.include?('.') ? :number : :int, number, line)
            i += number.length
          elsif c == '"'
            value, i = string(i, line)
            out << Token.new(:string, value, line)
          elsif (punct = PUNCTUATION.find { |p| @text[i, p.length] == p })
            out << Token.new(:punct, punct, line)
            i += punct.length
          else
            raise Error, "#{@file}:#{line}: unexpected #{c.inspect}"
          end
        end
        out << Token.new(:end, nil, line)
      end

      # A string literal: bytes up to the closing quote; \" and \\ escape.
      def string(start, line)
        value = +''
        i = start + 1
        loop do
          raise Error, "#{@file}:#{line}: a string does not end" if i >= @text.length || @text[i] == "\n"

          c = @text[i]
          if c == '"'
            return [value, i + 1]
          elsif c == '\\' && ['"', '\\'].include?(@text[i + 1])
            value << @text[i + 1]
            i += 2
          else
            value << c
            i += 1
          end
        end
      end
    end

    # ------------------------------------------------------------------------
    # Parser: the grammar of docs/matcher.md, 3.1, into plain structs.

    EventDecl = Struct.new(:kind, :action, :arity, :close, :item, :fields, :line, keyword_init: true)
    ShapeDecl = Struct.new(:name, :params, :line, keyword_init: true)
    # MUTATION: a rewrite the compiler performs where its rules match.
    RewriteDecl = Struct.new(:name, :line, keyword_init: true)
    NonterminalDecl = Struct.new(:name, :attributes, :line, keyword_init: true)
    Rule = Struct.new(:name, :terms, :pattern, :cost, :line, keyword_init: true)
    # An event pattern: its kind, tests [[field, term]], and children (nil
    # when it has no parentheses).
    EventPattern = Struct.new(:kind, :tests, :children, :line, keyword_init: true)
    NonterminalPattern = Struct.new(:name, :terms, :line, keyword_init: true)
    # MUTATION: in a rewrite's rules, `_`: any one node; `...`: whatever
    # else a bracket holds, up to its close.
    AnyPattern = Struct.new(:line, keyword_init: true)
    RestPattern = Struct.new(:line, keyword_init: true)
    # A term: a variable (:variable, name), or a constant (:int, :number,
    # :string, :flag, :operator).
    Term = Struct.new(:type, :value, :line)

    AST = Struct.new(:events, :shapes, :nonterminals, :rules, :rewrites) # MUTATION: rewrites

    class Parser
      def initialize(tokens, file)
        @tokens = tokens
        @file = file
        @at = 0
        @wildcards = 0
      end

      def parse
        ast = AST.new([], [], [], [], []) # MUTATION: rewrites
        until peek.type == :end
          case peek.value
          when 'event' then ast.events << event
          when 'shape' then ast.shapes << shape
          when 'rewrite' then ast.rewrites << rewrite # MUTATION
          when 'nonterminal' then ast.nonterminals << nonterminal
          else ast.rules << rule
          end
        end
        ast
      end

      private

      def peek = @tokens[@at]

      def take = @tokens[@at].tap { @at += 1 }

      def fail(message, token = peek) = raise(Error, "#{@file}:#{token.line}: #{message}")

      def describe(token) = token.type == :end ? 'the end of the file' : token.value.inspect

      def expect(value)
        token = take
        fail("expected #{value.inspect}, found #{describe(token)}", token) unless token.value == value && token.type != :string
        token
      end

      def accept(value)
        return false unless peek.value == value && peek.type != :string

        @at += 1
        true
      end

      def word(type, what)
        token = take
        fail("expected #{what}, found #{describe(token)}", token) unless token.type == type
        token
      end

      def integer
        token = take
        fail("expected a number, found #{describe(token)}", token) unless token.type == :int
        token.value.to_i
      end

      def event
        line = take.line
        kind = word(:upper, 'an event kind').value
        action = word(:lower, 'an action').value
        decl = EventDecl.new(kind: kind, item: false, fields: [], line: line, arity: 0)
        case action
        when 'leaf' then decl.action = :leaf
        when 'postfix'
          if accept('count')
            decl.action = :postfix_count
          else
            decl.action = :postfix
            decl.arity = integer
          end
        when 'open'
          decl.action = :open
          decl.arity = integer
          decl.close = word(:upper, 'the kind that closes it').value
        when 'inner' then decl.action = :inner
        when 'close' then decl.action = :close
        else fail("unknown action #{action.inspect}")
        end
        decl.item = true if accept('item')
        decl.fields = typed_list(FIELD_TYPES, 'a field') unless peek.value == ';'
        expect(';')
        decl
      end

      # `name: type, ...`, up to a `;` or a `)`.
      def typed_list(types, what)
        list = []
        loop do
          name = word(:lower, "#{what}'s name")
          expect(':')
          type = word(:lower, 'a type')
          fail("#{type.value.inspect} is not one of #{types.join(', ')}", type) unless types.include?(type.value)
          list << [name.value, type.value, name.line]
          break unless accept(',')
        end
        list
      end

      def shape
        line = take.line
        name = word(:upper, "a shape's name").value
        expect('(')
        params = peek.value == ')' ? [] : typed_list(PARAM_TYPES, 'a parameter')
        expect(')')
        expect(';')
        ShapeDecl.new(name: name, params: params, line: line)
      end

      # MUTATION: `rewrite NAME;`
      def rewrite
        line = take.line
        name = word(:upper, "a rewrite's name").value
        expect(';')
        RewriteDecl.new(name: name, line: line)
      end

      def nonterminal
        line = take.line
        name = word(:lower, "a nonterminal's name")
        fail("#{name.value} is a keyword", name) if KEYWORDS.include?(name.value)
        fail('names beginning with _ are the generator\'s', name) if name.value.start_with?('_')
        expect('(')
        attributes = peek.value == ')' ? [] : typed_list(FIELD_TYPES, 'an attribute')
        expect(')')
        expect(';')
        NonterminalDecl.new(name: name.value, attributes: attributes, line: line)
      end

      def rule
        head = take
        fail("expected a declaration or a rule, found #{describe(head)}", head) unless %i[upper lower].include?(head.type)
        expect('(')
        terms = peek.value == ')' ? [] : term_list
        expect(')')
        expect('<-')
        pattern = self.pattern
        cost = accept('cost') ? integer : 0
        expect(';')
        Rule.new(name: head.value, terms: terms, pattern: pattern, cost: cost, line: head.line)
      end

      def term_list
        list = [term]
        list << term while accept(',')
        list
      end

      def term
        token = take
        case token.type
        when :int then Term.new(:int, token.value.to_i, token.line)
        when :number then Term.new(:number, token.value.to_f, token.line)
        when :string then Term.new(:string, token.value, token.line)
        when :upper then Term.new(:operator, token.value, token.line)
        when :lower
          case token.value
          when 'true', 'false' then Term.new(:flag, token.value == 'true' ? 1 : 0, token.line)
          when '_' then Term.new(:variable, "_#{@wildcards += 1}", token.line)
          else
            fail("#{token.value} is a keyword", token) if KEYWORDS.include?(token.value)
            Term.new(:variable, token.value, token.line)
          end
        else fail("expected a variable or a constant, found #{describe(token)}", token)
        end
      end

      def pattern
        token = take
        case token.type
        when :upper
          tests = []
          if accept('{')
            loop do
              field = word(:lower, 'a field')
              expect(':')
              tests << [field.value, term]
              break unless accept(',')
            end
            expect('}')
          end
          children = nil
          if accept('(')
            children = []
            unless peek.value == ')'
              children << child_pattern # MUTATION: child_pattern, which reads ...
              children << child_pattern while accept(',')
            end
            expect(')')
          end
          EventPattern.new(kind: token.value, tests: tests, children: children, line: token.line)
        when :lower
          fail("#{token.value} is a keyword", token) if KEYWORDS.include?(token.value)
          return AnyPattern.new(line: token.line) if token.value == '_' # MUTATION

          terms = []
          if accept('(')
            terms = term_list unless peek.value == ')'
            expect(')')
          end
          NonterminalPattern.new(name: token.value, terms: terms, line: token.line)
        else fail("expected a pattern, found #{describe(token)}", token)
        end
      end

      # MUTATION: a child, or `...`, the rest of a bracket's children.
      def child_pattern
        return RestPattern.new(line: take.line) if peek.value == '...' && peek.type == :punct

        pattern
      end
    end

    # ------------------------------------------------------------------------
    # Checker: every check of docs/matcher.md, 4.3. Answers the model the
    # normaliser and the emitter read.

    # What a type is compared as: name and read are texts.
    # An operand is a literal's value (docs/matcher.md, 11.2): kept as bytes,
    # as a text is, but compared only with operands.
    CLASSES = { 'count' => :count, 'operator' => :operator, 'number' => :number, 'text' => :text, 'name' => :text,
                'read' => :text, 'flag' => :flag, 'operand' => :operand }.freeze

    # MUTATION: `rewrites` and `anchored`, the nonterminals a rewrite's rules
    # use, whose rules (`anchored_rules`) the normaliser leaves alone.
    Model = Struct.new(:kinds, :operators, :events, :shapes, :nonterminals, :rules, :rewrites, :anchored,
                       :anchored_rules, keyword_init: true)

    class Checker
      def initialize(ast, kinds, operators, file)
        @ast = ast
        @kinds = kinds
        @operators = operators
        @file = file
      end

      def fail(line, message) = raise(Error, "#{@file}:#{line}: #{message}")

      def check
        events = check_events
        @events = events
        shapes = {}
        @ast.shapes.each do |s|
          fail(s.line, "shape #{s.name} is declared twice") if shapes.key?(s.name)
          s.params.each { |name, _type, line| fail(line, "#{s.name} has two parameters #{name}") if s.params.count { |p| p[0] == name } > 1 }
          shapes[s.name] = s
        end
        nonterminals = {}
        @ast.nonterminals.each do |n|
          fail(n.line, "nonterminal #{n.name} is declared twice") if nonterminals.key?(n.name)
          nonterminals[n.name] = n
        end
        @shapes = shapes
        @nonterminals = nonterminals
        @rewrites = check_rewrites # MUTATION
        @anchored = anchored_names
        @anchored.each do |name|
          decl = @nonterminals[name]
          next unless decl && !decl.attributes.empty?

          fail(decl.line, "nonterminal #{name} serves a rewrite's rules, which hand up nothing: it has no attributes")
        end
        anchored_rules, rules = @ast.rules.partition { |rule| anchored?(rule.name) }
        rules.each { |rule| check_rule(rule) }
        anchored_rules.each { |rule| check_anchored_rule(rule) }
        check_use
        check_chains
        # MUTATION: the anchored nonterminals and rules apart.
        Model.new(kinds: @kinds, operators: @operators, events: events, shapes: shapes,
                  nonterminals: nonterminals.reject { |name, _| @anchored.include?(name) }, rules: rules,
                  rewrites: @rewrites, anchored: @anchored, anchored_rules: anchored_rules)
      end

      private

      def check_events
        events = {}
        @ast.events.each do |e|
          fail(e.line, "#{e.kind} is not an sjs_event_kind of staticjs.h") unless @kinds.include?(e.kind)
          fail(e.line, "#{e.kind} is declared twice") if events.key?(e.kind)
          e.fields.each do |name, type, line|
            fail(line, "#{e.kind} has two fields #{name}") if e.fields.count { |f| f[0] == name } > 1
            fail(line, "#{e.kind}'s #{name} is an operand, which only a leaf event has") if type == 'operand' && e.action != :leaf
          end
          events[e.kind] = e
        end
        missing = @kinds - events.keys
        fail(@ast.events.last&.line || 1, "#{missing.join(', ')} not declared") unless missing.empty?
        closers = Hash.new { |h, k| h[k] = [] }
        events.each_value do |e|
          next unless e.action == :open

          fail(e.line, "#{e.kind} is closed by #{e.close}, which is not declared close") unless events[e.close]&.action == :close
          closers[e.close] << e.kind
        end
        fail(events['END'].line, 'END must be declared close: it closes the program') unless events['END'].action == :close
        events.each_value do |e|
          next unless e.action == :close && e.kind != 'END'

          count = closers[e.kind].size
          fail(e.line, "#{e.kind} closes #{count.zero? ? 'no open kind' : closers[e.kind].join(' and ')}") unless count == 1
        end
        fail(events['END'].line, "END closes the program, not #{closers['END'].join(', ')}") unless closers['END'].empty?
        events
      end

      # MUTATION: rewrites (docs/matcher.md, section 12): declared once, of
      # names no shape has.
      def check_rewrites
        rewrites = {}
        @ast.rewrites.each do |r|
          fail(r.line, "rewrite #{r.name} is declared twice") if rewrites.key?(r.name)
          fail(r.line, "#{r.name} is a shape and a rewrite") if @shapes.key?(r.name)
          rewrites[r.name] = r
        end
        rewrites
      end

      def anchored?(name) = @rewrites.key?(name) || @anchored.include?(name)

      # The nonterminals a rewrite's rules use, and the ones their rules
      # use, and so on: they are matched by the anchored lane, not the
      # labeller, so no shape's rule may use one.
      def anchored_names
        names = []
        used = lambda do |p|
          case p
          when NonterminalPattern then [p.name]
          when EventPattern then (p.children || []).flat_map { |c| used.call(c) }
          else []
          end
        end
        loop do
          grown = @ast.rules.select { |r| @rewrites.key?(r.name) || names.include?(r.name) }
                            .flat_map { |r| used.call(r.pattern) }.uniq - names
          break if grown.empty?

          names.concat(grown)
        end
        @ast.rules.each do |r|
          next if @rewrites.key?(r.name) || names.include?(r.name)

          shared = used.call(r.pattern) & names
          fail(r.line, "#{shared.first} serves a rewrite's rules and #{r.name}'s: give each its own") unless shared.empty?
        end
        names
      end

      # A rule of a rewrite, or of a nonterminal one uses: anchored at the
      # node's start, so that the lane matches it anywhere, in a dead region
      # too (docs/matcher.md, 12.2). It is a leaf; a postfix event whose
      # first operand derives an anchored nonterminal and whose others are
      # `_`; a bracket that takes the node before it, which derives one,
      # then `...`; or a chain rule. Its tests compare with constants; it
      # binds nothing and hands up nothing.
      def check_anchored_rule(rule)
        declared = @rewrites[rule.name] || @nonterminals[rule.name]
        fail(rule.line, "#{rule.name} is not a declared #{rule.name.match?(/\A[A-Z]/) ? 'rewrite' : 'nonterminal'}") unless declared
        fail(rule.line, "#{rule.name} hands up nothing: a rewrite's rules have no attributes") unless rule.terms.empty?
        fail(rule.line, "a rewrite's rules have no cost") if rule.cost.positive?
        p = rule.pattern
        return anchored_child(p, rule) if p.is_a?(NonterminalPattern)
        fail(p.line, "#{rule.name}'s pattern is an event or a nonterminal") unless p.is_a?(EventPattern)

        e = @events[p.kind] or fail(p.line, "#{p.kind} is not an event kind")
        p.tests.each do |field, term|
          f = e.fields.find { |name, _| name == field } or fail(term.line, "#{p.kind} has no field #{field}")
          fail(term.line, "a rewrite's rule tests against constants only, not #{term.value}") if term.type == :variable
          constant(term, f[1], "#{p.kind}'s #{field}")
        end
        children = p.children || []
        case e.action
        when :leaf
          fail(p.line, "#{p.kind} has no children") unless children.empty?
        when :postfix
          unless children.size == e.arity
            fail(p.line, "#{p.kind} takes #{e.arity} child pattern(s), not #{children.size}")
          end
          anchored_child(children[0], rule)
          children.drop(1).each do |c|
            fail(c.line, "a rewrite's rule names only the first operand of #{p.kind}: write _ for the others") unless c.is_a?(AnyPattern)
          end
        when :open
          unless e.arity == 1 && children.size == 2 && children[1].is_a?(RestPattern)
            fail(p.line, "a rewrite's bracket takes the node before it and then `...`: #{p.kind}(n, ...)")
          end
          anchored_child(children[0], rule)
        else
          fail(p.line, "#{p.kind} cannot be anchored: a rewrite's rule is a leaf, a postfix event or a bracket that takes the node before it")
        end
      end

      def anchored_child(p, rule)
        fail(p.line, "a rewrite (#{p.kind}) cannot be a child") if p.is_a?(EventPattern) && @rewrites.key?(p.kind)
        unless p.is_a?(NonterminalPattern)
          fail(p.line, "the first child of a rewrite's rule is a nonterminal: write a nested pattern as a rule of its own")
        end
        fail(p.line, "a rewrite (#{p.name}) cannot be a child") if @rewrites.key?(p.name)
        fail(p.line, "#{p.name} is not a declared nonterminal") unless @nonterminals.key?(p.name)
        fail(p.line, "#{p.name} hands up nothing: a rewrite's rules have no attributes") unless p.terms.empty?
        fail(p.line, "#{rule.name} is a rewrite's: #{p.name} is not") unless @anchored.include?(p.name)
      end

      # The variables of a rule: name => [[class, type, line], ...].
      def check_rule(rule)
        @variables = Hash.new { |h, k| h[k] = [] }
        shape = rule.name.match?(/\A[A-Z]/)
        declared = shape ? @shapes[rule.name] : @nonterminals[rule.name]
        fail(rule.line, "#{rule.name} is not a declared #{shape ? 'shape' : 'nonterminal'}") unless declared
        expected = shape ? declared.params : declared.attributes
        unless rule.terms.size == expected.size
          fail(rule.line, "#{rule.name} has #{expected.size} attribute(s), not #{rule.terms.size}")
        end
        fail(rule.line, "a cost is at most #{MAX_COST}") if rule.cost > MAX_COST
        if shape && !(rule.pattern.is_a?(EventPattern) && rule.pattern.kind == 'ARROW_BEGIN')
          fail(rule.line, "the pattern of shape #{rule.name} is an ARROW_BEGIN: shapes are recorded on functions")
        end
        pattern(rule.pattern, :root)
        heads = []
        rule.terms.zip(expected).each do |term, (name, type)|
          if term.type == :variable
            fail(term.line, "#{term.value} is not bound by the pattern") unless @variables.key?(term.value)
            fail(term.line, "a wildcard cannot be an attribute") if term.value.start_with?('_')
            heads << [term.value, type]
            occurrence(term, type, :head)
          else
            fail(term.line, "a head's text comes from its pattern, not a constant") if CLASSES[type] == :text
            fail(term.line, 'an operand comes from its pattern, never a constant') if CLASSES[type] == :operand
            constant(term, type, "#{rule.name}'s #{name}")
          end
        end
        closure(rule, shape, heads)
      end

      # A read name must be the arrow's own: bound by a name in the same rule,
      # or handed up as a read attribute for a rule above to bind.
      def closure(rule, shape, heads)
        @variables.each do |variable, uses|
          next unless uses.any? { |_, type, _, where| type == 'read' && where != :head }
          next if uses.any? { |_, type| type == 'name' }

          line = uses.first[2]
          fail(line, "#{rule.name} reads #{variable}, which no BIND in it binds: it could be a captured name") if shape
          exported = heads.select { |name, _| name == variable }
          if exported.empty? || exported.any? { |_, type| type != 'read' }
            fail(line, "#{rule.name} reads #{variable} without binding it or handing it up as a read attribute")
          end
        end
      end

      def occurrence(term, type, where)
        uses = @variables[term.value]
        cls = CLASSES.fetch(type)
        if (first = uses.first) && first[0] != cls
          fail(term.line, "#{term.value} is #{article(type)} here and #{article(first[1])} at line #{first[2]}")
        end
        uses << [cls, type, term.line, where]
      end

      def article(type) = "#{type.match?(/\A[aeiou]/) ? 'an' : 'a'} #{type}"

      def constant(term, type, what)
        fail(term.line, 'an operand comes from its pattern, never a constant') if CLASSES.fetch(type) == :operand
        ok = case CLASSES.fetch(type)
             when :count then term.type == :int
             when :operator then term.type == :operator && @operators.include?(term.value)
             when :number then %i[int number].include?(term.type)
             when :text then term.type == :string
             when :flag then term.type == :flag
             end
        return if ok
        if term.type == :operator && CLASSES[type] == :operator
          fail(term.line, "#{term.value} is not an sjs_operator")
        end
        fail(term.line, "#{what} is #{article(type)}, not #{term.value.inspect}")
      end

      # `where`: :root, :child (a node in a bracket's sequence or a postfix's
      # operand), :sequence (anything in a bracket's sequence).
      def pattern(p, where)
        # MUTATION: wildcards are a rewrite's.
        fail(p.line, "`_` and `...` are in a rewrite's rules only") if p.is_a?(AnyPattern) || p.is_a?(RestPattern)
        if p.is_a?(NonterminalPattern)
          fail(p.line, "a shape (#{p.name}) cannot be a child") if @shapes.key?(p.name)
          fail(p.line, "a rewrite (#{p.name}) cannot be a child") if @rewrites.key?(p.name) # MUTATION
          decl = @nonterminals[p.name] or fail(p.line, "#{p.name} is not a declared nonterminal")
          unless p.terms.size == decl.attributes.size
            fail(p.line, "#{p.name} has #{decl.attributes.size} attribute(s), not #{p.terms.size}")
          end
          p.terms.zip(decl.attributes).each { |term, (name, type)| test_term(term, type, "#{p.name}'s #{name}") }
          return
        end
        fail(p.line, "a shape (#{p.kind}) cannot be a child") if @shapes.key?(p.kind)
        fail(p.line, "a rewrite (#{p.kind}) cannot be a child") if @rewrites.key?(p.kind) # MUTATION
        e = @events[p.kind] or fail(p.line, "#{p.kind} is not an event kind")
        fail(p.line, "#{p.kind} closes a bracket and is never in a pattern") if e.action == :close
        if e.action == :open && e.arity > 1
          # The matcher folds the one cell before such an open into its
          # marker as its first child (docs/matcher.md, 11.9.2); more it
          # would have to move.
          fail(p.line, "#{p.kind} takes the #{e.arity} nodes before it as its first children; the matcher takes one")
        end
        if e.action == :open && e.arity == 1 && p.children
          first = p.children.first
          if first.nil? || (first.is_a?(EventPattern) && @events[first.kind]&.action == :inner)
            fail(p.line, "#{p.kind}'s first child is the node before it: a pattern of a node, not a separator")
          end
        end
        if e.action == :inner && where != :sequence
          fail(p.line, "#{p.kind} is a separator: it is only in a bracket's child list")
        end
        p.tests.each do |field, term|
          f = e.fields.find { |name, _| name == field } or fail(term.line, "#{p.kind} has no field #{field}")
          test_term(term, f[1], "#{p.kind}'s #{field}")
        end
        children = p.children
        case e.action
        when :leaf, :inner
          fail(p.line, "#{p.kind} has no children") unless children.nil? || children.empty?
        when :postfix
          unless children&.size == e.arity
            fail(p.line, "#{p.kind} takes #{e.arity} child pattern(s), not #{children&.size || 0}")
          end
        when :postfix_count
          fail(p.line, "#{p.kind} takes one child pattern or none") if children && children.size > 1
        when :open
          fail(p.line, "#{p.kind} opens a bracket: list its children in parentheses") if children.nil?
        end
        (children || []).each { |child| pattern(child, e.action == :open ? :sequence : :child) }
      end

      def test_term(term, type, what)
        if term.type == :variable
          occurrence(term, type, :pattern)
        else
          constant(term, type, what)
        end
      end

      # Every shape and every nonterminal is derived by a rule and, but for
      # shapes, used by one.
      def check_use
        derived = @ast.rules.map(&:name)
        used = []
        walk = lambda do |p|
          if p.is_a?(NonterminalPattern)
            used << p.name
          elsif p.is_a?(EventPattern) # MUTATION: not a wildcard
            (p.children || []).each { |c| walk.call(c) }
          end
        end
        @ast.rules.each { |r| walk.call(r.pattern) }
        @shapes.each_value { |s| fail(s.line, "shape #{s.name} has no rule") unless derived.include?(s.name) }
        @rewrites.each_value { |r| fail(r.line, "rewrite #{r.name} has no rule") unless derived.include?(r.name) } # MUTATION
        @nonterminals.each_value do |n|
          fail(n.line, "nonterminal #{n.name} has no rule") unless derived.include?(n.name)
          fail(n.line, "nonterminal #{n.name} is never used") unless used.include?(n.name)
        end
      end

      def check_chains
        edges = {}
        @ast.rules.each { |r| (edges[r.name] ||= []) << [r.pattern.name, r.line] if r.pattern.is_a?(NonterminalPattern) }
        state = {}
        visit = lambda do |name, line, path|
          return if state[name] == :done
          fail(line, "chain rules form a cycle: #{(path + [name]).join(' <- ')}") if state[name] == :open

          state[name] = :open
          edges.fetch(name, []).each { |child, at| visit.call(child, at, path + [name]) }
          state[name] = :done
        end
        edges.each { |name, list| visit.call(name, list.first[1], []) }
      end
    end

    # ------------------------------------------------------------------------
    # Normaliser: every rule in one of four forms (docs/matcher.md, 4.2).

    # A normalised rule. `tests`: [field, type class, variable index or nil,
    # bind, constant]. `children`: { nonterminal:, links: [[variable, bind]] }
    # or { inner:, tests: }. `results`: [variable index or nil, constant].
    NormalRule = Struct.new(:lhs, :form, :kind, :tests, :children, :variables, :results, :cost, :order,
                            keyword_init: true)
    Nonterminal = Struct.new(:id, :name, :attributes, keyword_init: true) # attributes: [[name, class]]
    Table = Struct.new(:nonterminals, :rules, :chains, keyword_init: true)

    class Normaliser
      def initialize(model)
        @model = model
        @nonterminals = []
        @by_name = {}
        @fresh = {}
        @rules = []
        model.shapes.each_value { |s| add_nonterminal(s.name, s.params.map { |n, t| [n, CLASSES[t]] }) }
        model.nonterminals.each_value { |n| add_nonterminal(n.name, n.attributes.map { |a, t| [a, CLASSES[t]] }) }
      end

      def run
        @model.rules.each do |rule|
          normalise(@by_name[rule.name], rule.terms, rule.pattern, rule.cost)
        end
        raise Error, "the rules need #{@nonterminals.size} nonterminals; the table holds #{MAX_NONTERMINALS - 1}" \
          if @nonterminals.size >= MAX_NONTERMINALS
        Table.new(nonterminals: @nonterminals, rules: @rules, chains: chain_order)
      end

      private

      def add_nonterminal(name, attributes)
        n = Nonterminal.new(id: @nonterminals.size + 1, name: name, attributes: attributes)
        @nonterminals << n
        @by_name[name] = n
        n
      end

      # The variables of a pattern, in the order the matcher meets them: a
      # node's own tests, then its children in order.
      def variables_of(p, out = [])
        if p.is_a?(NonterminalPattern)
          p.terms.each { |t| out << t.value if t.type == :variable && !out.include?(t.value) }
        else
          p.tests.each { |_, t| out << t.value if t.type == :variable && !out.include?(t.value) }
          (p.children || []).each { |c| variables_of(c, out) }
        end
        out
      end

      # The class of each variable, from where it occurs.
      def classes_of(p, out = {})
        if p.is_a?(NonterminalPattern)
          decl = @by_name.fetch(p.name)
          p.terms.zip(decl.attributes).each { |t, (_, cls)| out[t.value] ||= cls if t.type == :variable }
        else
          fields = @model.events.fetch(p.kind).fields
          p.tests.each do |field, t|
            out[t.value] ||= CLASSES[fields.find { |f| f[0] == field }[1]] if t.type == :variable
          end
          (p.children || []).each { |c| classes_of(c, out) }
        end
        out
      end

      # A pattern written with its variables renamed in the order met, so
      # that two patterns equal but for their names give the same text.
      def canonical(p, names)
        name = ->(t) { t.type == :variable ? "$#{names[t.value] ||= names.size}" : "#{t.type}:#{t.value}" }
        if p.is_a?(NonterminalPattern)
          "#{p.name}(#{p.terms.map(&name).join(',')})"
        else
          tests = p.tests.map { |field, t| "#{field}=#{name.call(t)}" }.join(',')
          children = p.children&.map { |c| canonical(c, names) }&.join(',')
          "#{p.kind}{#{tests}}#{children ? "(#{children})" : ''}"
        end
      end

      # The nonterminal deriving the nested pattern `p`, whose attributes are
      # `exported` (variables of `p`, in the order met): one already made for
      # an identical pattern, or a new one.
      def fresh(p, exported)
        names = {}
        key = canonical(p, names)
        key = "#{key}->#{exported.map { |v| names[v] }.join(',')}"
        return @fresh[key] if @fresh.key?(key)

        classes = classes_of(p)
        n = add_nonterminal("_#{@fresh.size + 1}", exported.map { |v| [v, classes.fetch(v)] })
        @fresh[key] = n
        normalise(n, exported.map { |v| Term.new(:variable, v, p.line) }, p, 0)
        n
      end

      def normalise(lhs, head_terms, pattern, cost)
        if pattern.is_a?(NonterminalPattern)
          child = { nonterminal: @by_name.fetch(pattern.name), terms: pattern.terms }
          return add_rule(lhs, :chain, nil, [], [child], head_terms, cost)
        end
        event = @model.events.fetch(pattern.kind)
        form = case event.action
               when :leaf then :leaf
               when :open then :bracket
               else (pattern.children || []).empty? ? :leaf : :postfix
               end
        children = (pattern.children || []).each_with_index.map do |child, i|
          if child.is_a?(NonterminalPattern)
            { nonterminal: @by_name.fetch(child.name), terms: child.terms }
          elsif @model.events.fetch(child.kind).action == :inner
            { inner: child.kind, tests: child.tests }
          else
            # A nested node: a nonterminal of its own, handing up the
            # variables that the rest of the rule uses.
            elsewhere = head_terms.select { |t| t.type == :variable }.map(&:value)
            pattern.tests.each { |_, t| elsewhere << t.value if t.type == :variable }
            pattern.children.each_with_index { |other, j| elsewhere.concat(variables_of(other)) unless j == i }
            exported = variables_of(child).select { |v| elsewhere.include?(v) }
            n = fresh(child, exported)
            { nonterminal: n, terms: exported.map { |v| Term.new(:variable, v, child.line) } }
          end
        end
        add_rule(lhs, form, pattern.kind, pattern.tests, children, head_terms, cost)
      end

      def add_rule(lhs, form, kind, tests, children, head_terms, cost)
        variables = []
        seen = lambda do |name|
          index = variables.index(name)
          return [index, false] if index

          variables << name
          [variables.size - 1, true]
        end
        fields = kind ? @model.events.fetch(kind).fields : []
        own = tests.map { |field, term| test_row(fields, field, term, seen) }
        normal_children = children.map do |child|
          if child[:inner]
            inner_fields = @model.events.fetch(child[:inner]).fields
            { inner: child[:inner], tests: child[:tests].map { |field, term| test_row(inner_fields, field, term, seen) } }
          else
            { nonterminal: child[:nonterminal], links: child[:terms].map { |t| seen.call(t.value) } }
          end
        end
        results = head_terms.map do |t|
          next [variables.index(t.value), nil] if t.type == :variable

          [nil, t.type == :operator ? "SJS_OP_#{t.value}" : t.value]
        end
        raise Error, "#{lhs.name} needs #{variables.size} variables; the table holds #{MAX_VARIABLES}" if variables.size > MAX_VARIABLES

        classes = {}
        own.each { |row| classes[row[2]] ||= row[1] if row[2] }
        normal_children.each do |child|
          if child[:inner]
            child[:tests].each { |row| classes[row[2]] ||= row[1] if row[2] }
          else
            child[:links].each_with_index { |(v, _), i| classes[v] ||= child[:nonterminal].attributes[i][1] }
          end
        end
        rule = NormalRule.new(lhs: lhs, form: form, kind: kind, tests: own, children: normal_children,
                              variables: variables.each_index.map { |v| classes.fetch(v) }, results: results,
                              cost: cost, order: @rules.size)
        @rules << rule
      end

      def test_row(fields, field, term, seen)
        cls = CLASSES.fetch(fields.find { |f| f[0] == field }[1])
        if term.type == :variable
          index, bind = seen.call(term.value)
          [field, cls, index, bind, nil]
        else
          value = case term.type
                  when :operator then "SJS_OP_#{term.value}"
                  when :number, :int then cls == :number ? term.value.to_f : term.value
                  else term.value
                  end
          [field, cls, nil, false, value]
        end
      end

      # Chain rules, each after every chain rule that derives its child.
      def chain_order
        chains = @rules.select { |r| r.form == :chain }
        ordered = []
        until chains.empty?
          ready = chains.find { |r| chains.none? { |other| other.lhs == r.children[0][:nonterminal] } }
          ordered << ready
          chains.delete(ready)
        end
        ordered
      end
    end

    # ------------------------------------------------------------------------
    # Emitter: the two headers, byte for byte the same for the same input.

    module Emitter
      module_function

      BANNER = "/* Generated by `mise run shapes` from src/shapes.rules (docs/matcher.md); do not edit. */\n"
      ARG_KINDS = { 'text' => 'SJS_SHAPE_ARG_TEXT', 'flag' => 'SJS_SHAPE_ARG_FLAG', 'count' => 'SJS_SHAPE_ARG_COUNT',
                    'number' => 'SJS_SHAPE_ARG_NUMBER', 'operator' => 'SJS_SHAPE_ARG_OPERATOR',
                    'operand' => 'SJS_SHAPE_ARG_OPERAND' }.freeze
      FIELDS = { count: 'SHAPE_FIELD_COUNT', operator: 'SHAPE_FIELD_COUNT', flag: 'SHAPE_FIELD_COUNT',
                 number: 'SHAPE_FIELD_NUMBER', text: 'SHAPE_FIELD_TEXT', operand: 'SHAPE_FIELD_OPERAND' }.freeze
      ACTIONS = { leaf: 'SHAPE_LEAF', postfix: 'SHAPE_POSTFIX', postfix_count: 'SHAPE_POSTFIX_COUNT', open: 'SHAPE_OPEN',
                  inner: 'SHAPE_INNER', close: 'SHAPE_CLOSE' }.freeze
      FORMS = { leaf: 'SHAPE_FORM_LEAF', postfix: 'SHAPE_FORM_POSTFIX', bracket: 'SHAPE_FORM_BRACKET',
                chain: 'SHAPE_FORM_CHAIN' }.freeze

      def shapes_h(model)
        shapes = model.shapes.values
        width = shapes.map { |s| s.name.length }.max.to_i + 'SJS_SHAPE_,'.length
        args = shapes.map { |s| s.params.size }.max.to_i
        out = +BANNER
        out << <<~C
          /*
           * The shapes the matcher tags functions with. A shape's arguments are
           * recorded on the function's sjs_function_info (src/internal.h).
           */
          #pragma once

          #include <stdint.h>

          typedef enum {
              SJS_SHAPE_NONE = 0,
        C
        shapes.each do |s|
          params = s.params.empty? ? 'no arguments' : s.params.map { |name, type, _| "#{name}: #{type}" }.join(', ')
          out << format("    %-#{width}s /* %s */\n", "SJS_SHAPE_#{s.name},", params)
        end
        out << <<~C
              SJS_SHAPE_COUNT
          } sjs_shape;

          /* The most arguments a shape has. */
          #define SJS_SHAPE_ARGS #{[args, 1].max}u

          /* What a shape's argument is: a text (recorded as a string constant), a
           * flag (0 or 1), a count, a number (a number constant), an operator (an
           * sjs_operator), or an operand (a literal's value: sjs_shape_operand). */
          enum {
              SJS_SHAPE_ARG_NONE = 0, SJS_SHAPE_ARG_TEXT, SJS_SHAPE_ARG_FLAG, SJS_SHAPE_ARG_COUNT, SJS_SHAPE_ARG_NUMBER,
              SJS_SHAPE_ARG_OPERATOR, SJS_SHAPE_ARG_OPERAND
          };

          /* Argument `i` of `shape`: SJS_SHAPE_ARG_NONE past its last. */
          static inline uint8_t sjs_shape_arg_kind(uint32_t shape, uint32_t i) {
              switch (shape) {
        C
        shapes.each do |s|
          next if s.params.empty? # the default: no arguments

          kinds = s.params.map { |_, type, _| ARG_KINDS.fetch(type) }
          out << "    case SJS_SHAPE_#{s.name}: {\n"
          out << "        static const uint8_t kinds[] = { #{kinds.join(', ')} };\n"
          out << "        return i < #{kinds.size}u ? kinds[i] : (uint8_t)SJS_SHAPE_ARG_NONE;\n"
          out << "    }\n"
        end
        out << <<~C
              default: return SJS_SHAPE_ARG_NONE;
              }
          }

          /* "PLUCK" and the like: for tests and messages. */
          static inline const char *sjs_shape_name(uint32_t shape) {
              switch (shape) {
        C
        shapes.each { |s| out << "    case SJS_SHAPE_#{s.name}: return \"#{s.name}\";\n" }
        out << <<~C
              default: return "NONE";
              }
          }
        C
        out << rewrites_h(model) # MUTATION
        operators = shapes.flat_map(&:params).any? { |_, type, _| type == 'operator' } ? model.operators : []
        return out if operators.empty?

        out << <<~C

          /* "STRICT_EQUAL" and the like, for an operator argument: for tests and
           * messages. */
          static inline const char *sjs_shape_operator_name(uint32_t op) {
              switch (op) {
        C
        operators.each { |o| out << "    case SJS_OP_#{o}: return \"#{o}\";\n" }
        out << <<~C
              default: return "NONE";
              }
          }
        C
        out
      end

      # MUTATION: the rewrites the compiler performs where their rules match
      # (docs/matcher.md, section 12).
      def rewrites_h(model)
        out = +<<~C

          /* The rewrites the matcher reports where their rules match: the compiler
           * compiles the node it names differently (docs/matcher.md, section 12). */
          typedef enum {
              SJS_REWRITE_NONE = 0,
        C
        model.rewrites.each_key { |name| out << "    SJS_REWRITE_#{name},\n" }
        out << <<~C
              SJS_REWRITE_COUNT
          } sjs_rewrite;

          /* "MUTATE_PATH" and the like: for tests and messages. */
          static inline const char *sjs_rewrite_name(uint32_t rewrite) {
              switch (rewrite) {
        C
        model.rewrites.each_key { |name| out << "    case SJS_REWRITE_#{name}: return \"#{name}\";\n" }
        out << <<~C
              default: return "NONE";
              }
          }
        C
        out
      end

      def table_h(model, table)
        Table.new(model, table).text
      end

      # The matcher's tables, laid out from the normalised rules.
      class Table
        def initialize(model, table)
          @model = model
          @table = table
          @tests = []
          @children = []
          @links = []
          @results = []
          @fields = []
          @numbers = []
          @texts = []
          layout
        end

        def nonterminal_count = @table.nonterminals.size + 1

        # Rules in table order: leaf and postfix rules by their event, bracket
        # rules by their open, each group as written; then the chain rules,
        # each after those that derive its child.
        def layout
          kinds = @model.kinds
          grouped = @table.rules.reject { |r| r.form == :chain }.sort_by { |r| [kinds.index(r.kind), r.order] }
          @rules = grouped + @table.chains
          @first = {}
          @rules.each_with_index do |r, i|
            next if r.form == :chain

            (@first[r.kind] ||= [i, 0])[1] += 1
          end
          bracket_counts = @first.select { |kind, _| @model.events[kind].action == :open }.transform_values { |_, n| n }
          if (over = bracket_counts.find { |_, n| n > MAX_BRACKET_RULES })
            raise Error, "#{over[0]} has #{over[1]} bracket rules; the table holds #{MAX_BRACKET_RULES} per open kind"
          end
          @max_bracket = bracket_counts.values.max.to_i
          # A cell's slots: each nonterminal's attributes, one after another.
          @slot = [0]
          slot = 0
          @table.nonterminals.each do |n|
            @slot << slot
            slot += n.attributes.size
          end
          @slot_fields = @table.nonterminals.flat_map { |n| n.attributes.map { |_, cls| FIELDS.fetch(cls) } }
          # A marker's slots: each bracket rule's variables, by open kind.
          marker_slots = Hash.new(0)
          @first_slot = {}
          @rules.each do |r|
            next unless r.form == :bracket

            @first_slot[r] = marker_slots[r.kind]
            marker_slots[r.kind] += r.variables.size
          end
          @slots = [@slot_fields.size, marker_slots.values.max.to_i, 1].max
          @variables = [@rules.map { |r| r.variables.size }.max.to_i, 1].max
          @rows = @rules.map { |r| row(r) }
          anchored_layout # MUTATION
        end

        # MUTATION: the anchored lane's rules (docs/matcher.md, 12.3): ids
        # from 1, the rewrites first; leaf, postfix and bracket rules by
        # their event, each group as written, then the chain rules, each
        # after those that derive its child. Their tests share shape_tests.
        def anchored_layout
          names = @model.rewrites.keys + @model.anchored
          if names.size >= MAX_ANCHORED_NONTERMINALS
            raise Error, "the rewrites need #{names.size} nonterminals; the table holds #{MAX_ANCHORED_NONTERMINALS - 1}"
          end

          @anchored_id = names.each_with_index.to_h { |name, i| [name, i + 1] }
          rules = @model.anchored_rules.each_with_index.to_a
          chains, grouped = rules.partition { |r, _| r.pattern.is_a?(NonterminalPattern) }
          kinds = @model.kinds
          grouped = grouped.sort_by { |r, i| [kinds.index(r.pattern.kind), i] }.map(&:first)
          ordered = []
          chains = chains.map(&:first)
          until chains.empty?
            ready = chains.find { |r| chains.none? { |other| other.name == r.pattern.name } }
            ordered << ready
            chains.delete(ready)
          end
          @anchored_chain_count = ordered.size
          @anchored_rules = grouped + ordered
          @anchored_first = {}
          grouped.each_with_index { |r, i| (@anchored_first[r.pattern.kind] ||= [i, 0])[1] += 1 }
          if (over = @anchored_first.find { |_, (_, n)| n > MAX_BRACKET_RULES })
            raise Error, "#{over[0]} has #{over[1][1]} rewrite rules; the table holds #{MAX_BRACKET_RULES} per kind"
          end
          @anchored_rows = @anchored_rules.map { |r| anchored_row(r) }
        end

        def anchored_row(rule)
          p = rule.pattern
          lhs = @anchored_id.fetch(rule.name)
          if p.is_a?(NonterminalPattern)
            return { lhs: lhs, form: FORMS.fetch(:chain), child: @anchored_id.fetch(p.name), test_count: 0, first_test: 0,
                     comment: "#{rule.name} <- #{p.name}" }
          end
          fields = @model.events.fetch(p.kind).fields
          first_test = @tests.size
          p.tests.each do |field, term|
            cls = CLASSES.fetch(fields.find { |f| f[0] == field }[1])
            value = case term.type
                    when :operator then "SJS_OP_#{term.value}"
                    when :number, :int then cls == :number ? term.value.to_f : term.value
                    else term.value
                    end
            @tests << test([field, cls, nil, false, value])
          end
          children = p.children || []
          action = @model.events.fetch(p.kind).action
          form = action == :open ? :bracket : (children.empty? ? :leaf : :postfix)
          child = children.empty? ? 0 : @anchored_id.fetch(children[0].name)
          tests = p.tests.map { |field, t| "#{field}: #{t.type == :string ? t.value.inspect : t.value}" }
          kids = children.map { |c| c.is_a?(NonterminalPattern) ? c.name : (c.is_a?(RestPattern) ? '...' : '_') }
          comment = "#{rule.name} <- #{p.kind}#{tests.empty? ? '' : "{#{tests.join(', ')}}"}"
          comment += "(#{kids.join(', ')})" unless kids.empty?
          { lhs: lhs, form: FORMS.fetch(form), child: child, test_count: p.tests.size, first_test: first_test,
            comment: comment.gsub('*/', '* /') }
        end

        def row(rule)
          first_test = @tests.size
          rule.tests.each { |t| @tests << test(t) }
          first_child = @children.size
          rule.children.each do |c|
            if c[:inner]
              first = @tests.size
              c[:tests].each { |t| @tests << test(t) }
              @children << [0, "SJS_EV_#{c[:inner]}", c[:tests].size, 0, first, 0]
            else
              first = @links.size
              c[:links].each { |v, bind| @links << [v, bind ? 1 : 0] }
              @children << [c[:nonterminal].id, 0, 0, c[:links].size, 0, first]
            end
          end
          first_result = @results.size
          rule.results.each_with_index do |(v, constant), i|
            if v
              @results << [v, 0]
            else
              cls = rule.lhs.attributes[i][1]
              @results << ['SHAPE_CONSTANT', cls == :number ? number_index(constant) : constant]
            end
          end
          first_variable = @fields.size
          rule.variables.each { |cls| @fields << FIELDS.fetch(cls) }
          {
            lhs: rule.lhs.id, form: FORMS.fetch(rule.form), kind: rule.kind ? "SJS_EV_#{rule.kind}" : '0',
            child_count: rule.children.size, test_count: rule.tests.size, first_slot: @first_slot.fetch(rule, 0),
            variable_count: rule.variables.size, cost: rule.cost, first_test: first_test, first_child: first_child,
            first_result: first_result, first_variable: first_variable, comment: comment(rule)
          }
        end

        def test((_field, cls, variable, bind, constant))
          value = case cls
                  when :number then variable ? 0 : number_index(constant)
                  when :text then variable ? 0 : text_index(constant)
                  else variable ? 0 : constant
                  end
          [FIELDS.fetch(cls), variable || 'SHAPE_CONSTANT', bind ? 1 : 0, value]
        end

        def number_index(value)
          @numbers << value unless @numbers.include?(value)
          @numbers.index(value)
        end

        def text_index(value)
          @texts << value unless @texts.include?(value)
          @texts.index(value)
        end

        # The rule, as the normalised form reads: `_2(x) <- NAME{name}`.
        def comment(rule)
          var = ->(i) { "v#{i}" }
          show = ->(v, c, cls) { v ? var.call(v) : (cls == :text ? c.inspect : c.to_s) }
          head = "#{rule.lhs.name}(#{rule.results.map { |v, c| show.call(v, c, nil) }.join(', ')})"
          tests = rule.tests.map { |field, cls, v, _, c| "#{field}: #{show.call(v, c, cls)}" }
          kids = rule.children.map do |c|
            if c[:inner]
              t = c[:tests].map { |field, cls, v, _, k| "#{field}: #{show.call(v, k, cls)}" }
              t.empty? ? c[:inner] : "#{c[:inner]}{#{t.join(', ')}}"
            else
              "#{c[:nonterminal].name}(#{c[:links].map { |v, _| var.call(v) }.join(', ')})"
            end
          end
          body = rule.form == :chain ? kids.first : "#{rule.kind}#{tests.empty? ? '' : "{#{tests.join(', ')}}"}"
          body += "(#{kids.join(', ')})" if %i[postfix bracket].include?(rule.form)
          text = "#{head} <- #{body}"
          text += " cost #{rule.cost}" if rule.cost.positive?
          text.gsub('*/', '* /')
        end

        def c_string(value)
          escaped = value.bytes.map do |b|
            if b == 0x22 || b == 0x5C then "\\#{b.chr}"
            elsif b.between?(0x20, 0x7E) then b.chr
            else format('\\%03o', b)
            end
          end.join
          "\"#{escaped}\""
        end

        def c_number(value) = value.to_f.to_s

        def text
          out = +BANNER
          shape_count = @model.shapes.size
          out << <<~C
            /*
             * The matcher's tables (docs/matcher.md, 4.5), included by src/matcher.c
             * alone. Nonterminal 0 is none; 1 to #{shape_count} are the shapes; then the
             * declared nonterminals, then the ones the generator made for nested
             * patterns (_1, _2, ...). A cell keeps every nonterminal's attributes in
             * its own slots; a marker keeps each bracket rule of its open kind's
             * variables, from the rule's first_slot.
             */
            #pragma once

            #define SHAPE_NONTERMINALS #{nonterminal_count}u
            #define SHAPE_SLOTS #{@slots}u
            #define SHAPE_VARIABLES #{@variables}u
            #define SHAPE_COSTS #{[nonterminal_count, @max_bracket].max}u
            #define SHAPE_TEXT_MAX #{TEXT_MAX}u
            #define SHAPE_INFINITE 0xFFFFu
            #define SHAPE_CONSTANT 0xFFu
            #define SHAPE_RULE_COUNT #{@rules.size}u
            #define SHAPE_CHAIN_COUNT #{@table.chains.size}u
            #define SHAPE_NUMBER_COUNT #{@numbers.size}u
            #define SHAPE_TEXT_COUNT #{@texts.size}u
            #define SHAPE_ANCHORED_NONTERMINALS #{@anchored_id.size + 1}u /* MUTATION: the anchored lane's */
            #define SHAPE_ANCHORED_RULE_COUNT #{@anchored_rules.size}u
            #define SHAPE_ANCHORED_CHAIN_COUNT #{@anchored_chain_count}u

            enum { SHAPE_LEAF = 1, SHAPE_POSTFIX, SHAPE_POSTFIX_COUNT, SHAPE_OPEN, SHAPE_INNER, SHAPE_CLOSE };
            enum { SHAPE_FIELD_COUNT = 1, SHAPE_FIELD_NUMBER, SHAPE_FIELD_TEXT, SHAPE_FIELD_OPERAND };
            enum { SHAPE_FORM_LEAF = 1, SHAPE_FORM_POSTFIX, SHAPE_FORM_BRACKET, SHAPE_FORM_CHAIN };

            /* How an event builds the tree, and which rules look at it. */
            typedef struct {
                uint8_t action;        /* SHAPE_LEAF ... SHAPE_CLOSE */
                uint8_t arity;         /* SHAPE_POSTFIX: its operands; SHAPE_OPEN: the cells before it it takes */
                uint8_t item;          /* its node is always a whole child of its bracket */
                uint8_t close;         /* SHAPE_OPEN: the sjs_event_kind that closes it */
                uint16_t first_rule;   /* its leaf and postfix rules, or its bracket rules if it opens */
                uint16_t rule_count;
            } shape_event;

            /* A test of one field of an event: bind `variable` (its first
             * occurrence) or compare with it, or compare with `constant` (a count,
             * an sjs_operator, or an index into shape_numbers or shape_texts). */
            typedef struct {
                uint8_t field;         /* SHAPE_FIELD_* */
                uint8_t variable;      /* or SHAPE_CONSTANT */
                uint8_t bind;
                uint32_t constant;
            } shape_test;

            /* A child in a rule: a cell deriving `nonterminal`, whose attributes are
             * linked to the rule's variables in order; or (nonterminal 0) the
             * separator `kind`, passing its tests. */
            typedef struct {
                uint8_t nonterminal;
                uint8_t kind;
                uint8_t test_count;
                uint8_t link_count;
                uint16_t first_test;
                uint16_t first_link;
            } shape_child;

            /* Attribute i of a child, bound to a variable or compared with it. */
            typedef struct {
                uint8_t variable;
                uint8_t bind;
            } shape_link;

            /* One attribute of what a rule derives: a variable, or a constant (a
             * count or a flag, or an index into shape_numbers). */
            typedef struct {
                uint8_t variable;      /* or SHAPE_CONSTANT */
                uint32_t constant;
            } shape_result;

            typedef struct {
                uint8_t lhs;           /* the nonterminal it derives */
                uint8_t form;          /* SHAPE_FORM_* */
                uint8_t kind;          /* the leaf's or postfix's event, or the bracket's open */
                uint8_t child_count;
                uint8_t test_count;    /* on the node's own event: the open's, for a bracket */
                uint8_t first_slot;    /* SHAPE_FORM_BRACKET: its variables' first slot in a marker */
                uint8_t variable_count;
                uint16_t cost;
                uint16_t first_test;
                uint16_t first_child;
                uint16_t first_result; /* one per attribute of lhs */
                uint16_t first_variable; /* in shape_variable_fields */
            } shape_rule;

            /* MUTATION: a rule of the anchored lane (docs/matcher.md, 12.3), which
             * matches a rewrite's rules: `child` is the anchored nonterminal its
             * first child derives (the operand a postfix event or a bracket takes
             * from before it; a chain rule's child), 0 for a leaf. Its tests, in
             * shape_tests, compare with constants. Nonterminal 0 is none; 1 to
             * SJS_REWRITE_COUNT - 1 are the rewrites. */
            typedef struct {
                uint8_t lhs;
                uint8_t form;          /* SHAPE_FORM_* */
                uint8_t child;
                uint8_t test_count;
                uint16_t first_test;
            } shape_anchored_rule;

            /* An event's anchored rules: its leaf or postfix rules, or its bracket's. */
            typedef struct {
                uint16_t first_rule;
                uint16_t rule_count;
            } shape_anchored_event;

          C
          out << "static const shape_event shape_events[SJS_EV_END + 1] = {\n"
          @model.kinds.each do |kind|
            e = @model.events.fetch(kind)
            first, count = @first.fetch(kind, [0, 0])
            close = e.action == :open ? "SJS_EV_#{e.close}" : '0'
            out << "    [SJS_EV_#{kind}] = { #{ACTIONS.fetch(e.action)}, #{e.arity}, #{e.item ? 1 : 0}, #{close}, #{first}, #{count} },\n"
          end
          out << "};\n\n"
          out << "static const shape_rule shape_rules[SHAPE_RULE_COUNT] = {\n"
          @rows.each_with_index do |r, i|
            out << "    /* #{i}: #{r[:comment]} */\n"
            out << "    { #{r[:lhs]}, #{r[:form]}, #{r[:kind]}, #{r[:child_count]}, #{r[:test_count]}, #{r[:first_slot]}, " \
                   "#{r[:variable_count]}, #{r[:cost]}, #{r[:first_test]}, #{r[:first_child]}, #{r[:first_result]}, " \
                   "#{r[:first_variable]} },\n"
          end
          out << "};\n\n"
          out << array('shape_test', 'shape_tests', @tests.map { |t| "{ #{t.join(', ')} }" }, '{ 0, 0, 0, 0 }')
          out << array('shape_child', 'shape_children', @children.map { |c| "{ #{c.join(', ')} }" }, '{ 0, 0, 0, 0, 0, 0 }')
          out << array('shape_link', 'shape_links', @links.map { |l| "{ #{l.join(', ')} }" }, '{ 0, 0 }')
          out << array('shape_result', 'shape_results', @results.map { |r| "{ #{r.join(', ')} }" }, '{ 0, 0 }')
          out << array('uint8_t', 'shape_variable_fields', @fields, '0')
          chains = @table.chains.map { |r| @rules.index(r).to_s }
          out << "/* The chain rules, each after those that derive its child. */\n"
          out << array('uint16_t', 'shape_chain_rules', chains, '0')
          out << "/* A nonterminal's first slot in a cell, and how many attributes it has. */\n"
          out << array('uint8_t', 'shape_slot', @slot.map(&:to_s), nil, size: 'SHAPE_NONTERMINALS')
          out << array('uint8_t', 'shape_attribute_count', ([0] + @table.nonterminals.map { |n| n.attributes.size }).map(&:to_s),
                       nil, size: 'SHAPE_NONTERMINALS')
          out << "/* What each slot of a cell holds. */\n"
          out << array('uint8_t', 'shape_slot_field', @slot_fields, '0')
          out << array('double', 'shape_numbers', @numbers.map { |n| c_number(n) }, '0.0')
          out << array('sjs_str', 'shape_texts', @texts.map { |t| "{ #{c_string(t)}, #{t.bytesize}u }" }, '{ 0, 0 }')
          out << anchored_text # MUTATION
          out.sub(/\n+\z/, "\n")
        end

        # MUTATION: the anchored lane's tables.
        def anchored_text
          out = +"/* MUTATION: the anchored lane's rules, which match the rewrites. */\n"
          out << "static const shape_anchored_event shape_anchored_events[SJS_EV_END + 1] = {\n"
          @model.kinds.each do |kind|
            first, count = @anchored_first.fetch(kind, [0, 0])
            out << "    [SJS_EV_#{kind}] = { #{first}, #{count} },\n" if count.positive?
          end
          out << "    { 0, 0 },\n" if @anchored_first.empty?
          out << "};\n\n"
          rows = @anchored_rows.each_with_index.map do |r, i|
            "/* #{i}: #{r[:comment]} */\n    { #{r[:lhs]}, #{r[:form]}, #{r[:child]}, #{r[:test_count]}, #{r[:first_test]} }"
          end
          out << "static const shape_anchored_rule shape_anchored_rules[] = {\n"
          rows = ['{ 0, 0, 0, 0, 0 }'] if rows.empty?
          rows.each { |row| out << "    #{row},\n" }
          out << "};\n\n"
          chains = @anchored_rules.each_index.drop(@anchored_rules.size - @anchored_chain_count).map(&:to_s)
          out << "/* The anchored chain rules, each after those that derive its child. */\n"
          out << array('uint16_t', 'shape_anchored_chain_rules', chains, '0')
          out
        end

        # A static array. C has no empty arrays: an unused one holds one
        # placeholder, which its count (SHAPE_*_COUNT, or a row's) never reaches.
        def array(type, name, items, placeholder, size: nil)
          items = [placeholder] if items.empty?
          out = +"static const #{type} #{name}[#{size || ''}] = {"
          if items.join(', ').length + out.length < 100
            out << " #{items.join(', ')} };\n\n"
          else
            out << "\n"
            items.each_slice(item_width(items)) { |slice| out << "    #{slice.join(', ')},\n" }
            out << "};\n\n"
          end
          out
        end

        def item_width(items) = items.first.start_with?('{') ? 4 : 6
      end
    end
  end
end
