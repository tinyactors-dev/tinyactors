# frozen_string_literal: true

module SCXML
  module IRP
    # A construct the importer cannot express for the jq data model. The test
    # that uses it is skipped with this message as the reason.
    class Unsupported < StandardError; end

    # Translates the IRP's datamodel-neutral `conf:` markup, and the small
    # ECMAScript expressions some tests write directly, into jq for the
    # `datamodel.jq` adapter.
    #
    # The jq data model keeps SCXML data as fields of the actor's root object, so
    # `Var1` is `.Var1`. System variables are jq variables (`$_event`,
    # `$_sessionid`, `$_name`, `$_ioprocessors`), and `In("id")` tests the
    # configuration. Evaluation errors are raised with jq's `error`, which the
    # data model reports as error.execution.
    #
    # The mapping follows vendor/scxml-irp/confEcma.xsl template by template.
    module JqExpressions
      SYSTEM_VARIABLES = %w[_event _sessionid _name _ioprocessors].freeze
      SCXML_PROCESSOR = 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor'
      # confEcma.xsl splits comparisons as ([0-9]+)([=<>]=?)(.*).
      COMPARISON = /\A([0-9]+)([=<>]=?)(.*)\z/m

      DATAMODEL = 'jq'

      module_function

      def datamodel = DATAMODEL

      def var(id) = "Var#{id}"

      # A data ID as a location, such as the outcome variable.
      def data_location(name) = ".#{name}"

      def location(id) = ".#{var(id)}"

      def string(text) = JSON.generate(text.to_s)

      def error(what) = "error(#{string("conf:#{what}")})"

      # Maps one `conf:` attribute to the SCXML attribute it stands for and that
      # attribute's value: jq source for expression attributes, plain text for
      # literal ones.
      def conf_attribute(name, value)
        case name
        when 'targetpass' then ['target', 'pass']
        when 'targetfail' then ['target', 'fail']
        when 'datamodel' then ['datamodel', 'jq']
        when 'id' then ['id', var(value)]
        when 'name' then ['name', var(value)]
        when 'location' then ['location', location(value)]
        when 'invalidLocation' then ['location', error(name)]
        when 'systemVarLocation' then ['location', system_variable(value)]
        when 'expr' then ['expr', ecmascript(value)]
        when 'targetVar', 'targetExpr' then ['targetexpr', location(value)]
        when 'quoteExpr' then ['expr', string(value)]
        when 'varExpr', 'varChildExpr' then ['expr', location(value)]
        when 'systemVarExpr' then ['expr', system_variable(value)]
        when 'varNonexistentStruct' then ['expr', "#{location(value)}.bar"]
        when 'illegalExpr' then ['expr', error(name)]
        when 'illegalTarget' then ['target', 'baz']
        when 'unreachableTarget' then ['target', '#_scxml_foo']
        when 'invalidSessionID' then ['expr', '27']
        when 'invalidSendType' then ['type', '27']
        when 'invalidSendTypeExpr' then ['expr', '27']
        when 'eventName' then ['expr', '$_event.name']
        when 'eventType' then ['expr', '$_event.type']
        when 'eventSendid' then ['expr', '$_event.sendid']
        when 'eventField' then ['expr', "$_event.#{field(value)}"]
        when 'eventRaw' then ['expr', '$_event.raw']
        when 'eventDataFieldValue', 'eventDataParamValue' then ['expr', "$_event.data.#{field(value)}"]
        when 'eventDataNamelistValue' then ['expr', "$_event.data.#{var(value)}"]
        when 'scxmlEventIOLocation' then ['expr', "$_ioprocessors[#{string(SCXML_PROCESSOR)}].location"]
        when 'eventExpr' then ['eventexpr', location(value)]
        when 'typeExpr' then ['typeexpr', location(value)]
        when 'delayFromVar' then ['delayexpr', location(value)]
        when 'delay' then ['delay', "#{value}s"]
        when 'idlocation' then ['idlocation', location(value)]
        when 'sendIDExpr' then ['sendidexpr', location(value)]
        when 'srcExpr' then ['srcexpr', location(value)]
        when 'namelist' then ['namelist', var(value)]
        when 'invalidNamelist' then ['namelist', '"foo']
        when 'idVal', 'namelistIdVal' then ['cond', comparison(value) { |rhs| ecmascript(rhs) }]
        when 'idQuoteVal' then ['cond', comparison(value) { |rhs| string(rhs) }]
        when 'idSystemVarVal' then ['cond', comparison(value) { |rhs| system_variable(rhs) }]
        when 'compareIDVal' then ['cond', comparison(value) { |rhs| location(rhs) }]
        when 'VarEqVar', 'VarEqVarStruct' then ['cond', var_eq_var(value)]
        when 'idSomeVal' then ['cond', "#{location(value)} == 123"]
        when 'eventNameVal' then ['cond', "$_event.name == #{string(value)}"]
        when 'eventvarVal' then ['cond', event_var_val(value)]
        when 'eventdataVal' then ['cond', "$_event.data == #{ecmascript(value)}"]
        when 'eventdataSomeVal' then ['cond', '$_event.data == 123']
        when 'emptyEventData' then ['cond', '$_event.data == null']
        when 'nameVarVal' then ['cond', "$_name == #{string(value)}"]
        when 'varPrefix' then ['cond', var_prefix(value)]
        when 'inState' then ['cond', "In(#{string(value)})"]
        when 'nonBoolean' then ['cond', error(name)]
        when 'isBound' then ['cond', location(value)]
        when 'unboundVar' then ['cond', "#{location(value)} == null"]
        when 'systemVarIsBound' then ['cond', "#{system_variable(value)} != null"]
        when 'noValue' then ['cond', "#{location(value)} | not"]
        when 'true' then %w[cond true]
        when 'false' then %w[cond false]
        when 'eventFieldsAreBound'
          fields = %w[name type sendid origin origintype invokeid data]
          ['cond', "$_event | #{fields.map { |f| "has(#{string(f)})" }.join(' and ')}"]
        when 'eventDataHasField' then ['cond', "$_event.data | has(#{string(value)})"]
        when 'eventFieldHasNoValue' then ['cond', "$_event.#{field(value)} == null"]
        when 'originTypeEq' then ['cond', "$_event.origintype == #{string(value)}"]
        when 'arrayVar', 'arrayTextVar' then ['array', location(value)]
        when 'illegalArray' then ['expr', '7']
        when 'item' then ['item', var(value)]
        when 'illegalItem' then ['item', "'continue'"]
        when 'index' then ['index', var(value)]
        when /\AbasicHTTP|\AmethodIs|\AeventIdParam|\AeventNamedParam|\AmessageBody|\AmsgIsBody|\AtestOnServer/
          raise Unsupported, "conf:#{name} needs the BasicHTTP Event I/O Processor"
        when 'scriptBadSrc', 'scriptGoodSrc'
          raise Unsupported, "conf:#{name} needs <script src>, which the importer would have to reject"
        else
          raise Unsupported, "conf:#{name} has no jq mapping"
        end
      end

      # The replacement text of `conf:` elements that stand for inline content.
      def conf_content(name)
        case name
        when 'someInlineVal' then '123'
        when 'array123' then '[1,2,3]'
        when 'contentFoo' then 'foo'
        when 'illegalContent' then ' xyz '
        else raise Unsupported, "conf:#{name} has no content mapping"
        end
      end

      # conf:incrementID, conf:concatVars, conf:sumVars, and conf:extendArray
      # as [location, expr] of an <assign>.
      def conf_assignment(name, attributes)
        case name
        when 'incrementID' then [location(attributes['id']), "#{location(attributes['id'])} + 1"]
        when 'concatVars', 'sumVars'
          [location(attributes['id1']), "#{location(attributes['id1'])} + #{location(attributes['id2'])}"]
        when 'extendArray' then [location(attributes['id']), "#{location(attributes['id'])} + [4]"]
        end
      end

      # The body of conf:script.
      def conf_script = '.Var1 = 1'

      # conf:sendToSender's targetexpr and typeexpr.
      def sender_exprs = %w[$_event.origin $_event.origintype]

      # Expressions and locations the document writes itself are ECMAScript;
      # small ones are translated.
      def plain_expression(source) = ecmascript(source)

      def plain_location(source) = ecmascript_location(source)

      # A namelist entry, used as a location. confEcma.xsl's invalid namelist
      # `"foo` becomes an evaluation error.
      def namelist_location(name) = name.start_with?('"') ? error('invalidNamelist') : ecmascript_location(name)

      # <script> bodies are ECMAScript, which jq cannot run.
      def script_body(text)
        raise Unsupported, "ECMAScript <script> #{text.to_s.strip.inspect}"
      end

      def system_variable(name)
        raise Unsupported, "unknown system variable #{name.inspect}" unless SYSTEM_VARIABLES.include?(name)

        "$#{name}"
      end

      def field(name)
        raise Unsupported, "event field #{name.inspect} is not an identifier" unless name.match?(/\A[A-Za-z_]\w*\z/)

        name
      end

      def comparison(value)
        match = COMPARISON.match(value) or raise Unsupported, "cannot parse comparison #{value.inspect}"
        operator = match[2] == '=' ? '==' : match[2]
        "#{location(match[1])} #{operator} #{yield match[3]}"
      end

      def var_eq_var(value)
        match = /\A([0-9]+)\W+([0-9]+)\z/.match(value) or raise Unsupported, "cannot parse #{value.inspect}"
        "#{location(match[1])} == #{location(match[2])}"
      end

      def event_var_val(value)
        match = COMPARISON.match(value) or raise Unsupported, "cannot parse comparison #{value.inspect}"
        operator = match[2] == '=' ? '==' : match[2]
        "$_event.data.#{var(match[1])} #{operator} #{ecmascript(match[3])}"
      end

      # confEcma.xsl: true when Var<b> (as a string) starts with Var<a>, for
      # "a b". An empty prefix always matches.
      def var_prefix(value)
        match = /\A(\w+)\W(\w+)\z/.match(value) or raise Unsupported, "cannot parse #{value.inspect}"
        "#{location(match[1])} as $prefix | #{location(match[2])} | " \
          '(tostring) as $text | ($prefix | tostring) as $start | $text | startswith($start)'
      end

      TOKEN = /\s*(?:
        (?<number>\d+(?:\.\d+)?|\.\d+) |
        (?<string>'[^']*'|"[^"]*") |
        (?<ident>[A-Za-z_$][\w$]*) |
        (?<punct>===|!==|==|!=|<=|>=|&&|\|\||[-+*\/<>()\[\],.!])
      )/x

      UNSUPPORTED_WORDS = %w[new typeof instanceof function return var delete void in this].freeze
      OPERATORS = { '===' => '==', '!==' => '!=', '&&' => 'and', '||' => 'or' }.freeze

      # Translates a small ECMAScript expression: literals, variables, member
      # and index access, arrays, arithmetic, comparisons, `&&`, `||`, and
      # `In(...)`. Anything else is unsupported.
      def ecmascript(source)
        text = source.to_s.strip
        raise Unsupported, 'empty expression' if text.empty?

        tokens = tokenize(text)
        out = +''
        tokens.each_with_index do |(kind, token), index|
          previous = index.positive? ? tokens[index - 1] : nil
          following = tokens[index + 1]
          case kind
          when :number
            out << ' ' unless out.empty? || out.end_with?('(', '[')
            out << (token.start_with?('.') ? "0#{token}" : token)
          when :string
            out << ' ' unless out.empty? || out.end_with?('(', '[')
            out << string(token[1..-2])
          when :ident
            if previous == [:punct, '.']
              raise Unsupported, "member #{token} in #{text.inspect}" unless token.match?(/\A[A-Za-z_]\w*\z/)

              out << token
              next
            end
            raise Unsupported, "ECMAScript #{token.inspect} in #{text.inspect}" if UNSUPPORTED_WORDS.include?(token)

            out << ' ' unless out.empty? || out.end_with?('(', '[')
            out << identifier(token, following, text)
          when :punct
            case token
            when '.'
              raise Unsupported, "member access in #{text.inspect}" unless following&.first == :ident

              out << '.'
            when '(', ')', '[', ']', ','
              if token == '[' && previous && %i[ident string].include?(previous.first) || previous == [:punct, ']']
                out << '['
              else
                out << (token == ',' ? ', ' : token)
              end
            when '!'
              raise Unsupported, "negation in #{text.inspect}"
            else
              out << " #{OPERATORS.fetch(token, token)}"
            end
          end
        end
        out.strip
      end

      def tokenize(text)
        tokens = []
        position = 0
        while position < text.length
          match = TOKEN.match(text, position)
          if match.nil? || match.begin(0) != position
            rest = text[position..].strip
            break if rest.empty?

            raise Unsupported, "cannot translate #{text.inspect} to jq"
          end
          kind = %i[number string ident punct].find { |name| match[name] }
          tokens << [kind, match[kind]]
          position = match.end(0)
        end
        tokens
      end

      def identifier(token, following, text)
        case token
        when 'true', 'false', 'null' then token
        when 'undefined' then 'null'
        when 'In'
          raise Unsupported, "In without a call in #{text.inspect}" unless following == [:punct, '(']

          'In'
        when *SYSTEM_VARIABLES then "$#{token}"
        else
          raise Unsupported, "call of #{token} in #{text.inspect}" if following == [:punct, '(']

          ".#{token}"
        end
      end

      # A location such as `Var1` or `foo.bar`.
      def ecmascript_location(source)
        text = source.to_s.strip
        return "$#{text}" if SYSTEM_VARIABLES.include?(text)
        raise Unsupported, "location #{text.inspect} is not a dotted name" unless text.match?(/\A[A-Za-z_]\w*(\.[A-Za-z_]\w*)*\z/)

        ".#{text}"
      end
    end
  end
end
