# frozen_string_literal: true

require_relative 'jq_expressions'

module SCXML
  module IRP
    # Translates the IRP's datamodel-neutral `conf:` markup into ECMAScript for
    # the `datamodel.ecmascript` adapter, for documents that declare
    # `datamodel="ecmascript"`.
    #
    # The mapping is vendor/scxml-irp/confEcma.xsl, template by template, so the
    # output is the text the W3C stylesheet produces. Expressions, locations,
    # conditions, and <script> bodies that the document writes directly are
    # ECMAScript already and pass through verbatim.
    #
    # The data model keeps SCXML data as ECMAScript variables, so `Var1` is
    # `Var1`. System variables are the ECMAScript variables `_event`,
    # `_sessionid`, `_name`, and `_ioprocessors`, and `In('id')` tests the
    # configuration.
    module EcmaExpressions
      DATAMODEL = 'ecmascript'
      SCXML_PROCESSOR = JqExpressions::SCXML_PROCESSOR
      # confEcma.xsl splits comparisons as ([0-9]+)([=<>]=?)(.*).
      COMPARISON = JqExpressions::COMPARISON
      # The adapter has no XML DOM: XML values stay text.
      DOM_METHODS = /\b(getElementsByTagName|getAttribute|childNodes|documentElement|firstChild|nodeValue)\b/

      # confEcma.xsl: conf:varPrefix. The underscore.string startsWith
      # function, compressed into one line, applied to (Var<b>, Var<a>).
      STARTS_WITH = "(function(str, starts){if (starts === '') return true;if (str == null || starts == null) " \
                    'return false;str = String(str); starts = String(starts);return str.length >= starts.length ' \
                    '&& str.slice(0, starts.length) === starts;})'

      module_function

      def datamodel = DATAMODEL

      def var(id) = "Var#{id}"

      # A conf: variable as a location or expression.
      def location(id) = var(id)

      # A data ID as a location, such as the outcome variable.
      def data_location(name) = name.to_s

      # An ECMAScript string literal, quoted as confEcma.xsl quotes them.
      def string(text) = "'#{text.to_s.gsub(/[\\']/) { |c| "\\#{c}" }}'"

      # Maps one `conf:` attribute to the SCXML attribute it stands for and that
      # attribute's value, as confEcma.xsl does.
      def conf_attribute(name, value)
        case name
        when 'targetpass' then ['target', 'pass']
        when 'targetfail' then ['target', 'fail']
        when 'datamodel' then ['datamodel', DATAMODEL]
        when 'id' then ['id', var(value)]
        when 'name' then ['name', var(value)]
        when 'location' then ['location', var(value)]
        when 'invalidLocation' then ['location', 'foo.bar.baz ']
        when 'systemVarLocation' then ['location', value]
        when 'expr' then ['expr', value]
        when 'targetVar', 'targetExpr' then ['targetexpr', var(value)]
        when 'quoteExpr' then ['expr', "'#{value}'"]
        when 'varExpr', 'varChildExpr' then ['expr', var(value)]
        when 'systemVarExpr' then ['expr', value]
        when 'varNonexistentStruct' then ['expr', "#{var(value)}.bar"]
        when 'illegalExpr' then %w[expr return]
        when 'illegalTarget' then ['target', 'baz']
        when 'unreachableTarget' then ['target', '#_scxml_foo']
        when 'invalidSessionID' then ['expr', '27']
        when 'invalidSendType' then ['type', '27']
        when 'invalidSendTypeExpr' then ['expr', '27']
        when 'eventName' then ['expr', '_event.name']
        when 'eventType' then ['expr', '_event.type']
        when 'eventSendid' then ['expr', '_event.sendid']
        when 'eventField' then ['expr', "_event.#{value}"]
        when 'eventRaw' then ['expr', '_event.raw']
        when 'eventDataFieldValue', 'eventDataParamValue' then ['expr', "_event.data.#{value}"]
        when 'eventDataNamelistValue' then ['expr', "_event.data.#{var(value)}"]
        when 'scxmlEventIOLocation' then ['expr', "_ioprocessors['#{SCXML_PROCESSOR}'].location"]
        when 'eventExpr' then ['eventexpr', var(value)]
        when 'typeExpr' then ['typeexpr', var(value)]
        when 'delayFromVar' then ['delayexpr', var(value)]
        when 'delay' then ['delayexpr', "'#{value}s'"]
        when 'idlocation' then ['idlocation', var(value)]
        when 'sendIDExpr' then ['sendidexpr', var(value)]
        when 'srcExpr' then ['srcexpr', var(value)]
        when 'namelist' then ['namelist', var(value)]
        when 'invalidNamelist' then ['namelist', '"foo']
        when 'idVal', 'idSystemVarVal' then ['cond', comparison(value, '==') { |rhs| rhs }]
        when 'varIdVal' then ['cond', comparison(value, '==') { |rhs| var(rhs) }]
        when 'namelistIdVal' then ['cond', comparison(value, '===') { |rhs| rhs }]
        when 'idQuoteVal' then ['cond', comparison(value, '==') { |rhs| "'#{rhs}'" }]
        # confEcma.xsl maps '=' to '=' here (an assignment), and matches a
        # single character on the right. Kept as the stylesheet has it.
        when 'compareIDVal'
          ['cond', comparison(value, '=', pattern: /\A([0-9]+)([=<>]=?)([0-9+])\z/) { |rhs| var(rhs) }]
        when 'VarEqVar' then ['cond', var_eq_var(value, '===')]
        when 'VarEqVarStruct' then ['cond', var_eq_var(value, '==')]
        when 'idSomeVal' then ['cond', "#{var(value)} == 123"]
        when 'eventNameVal' then ['cond', "_event.name == '#{value}'"]
        when 'eventvarVal' then ['cond', comparison(value, '==', left: ->(id) { "_event.data['#{var(id)}']" }) { |rhs| rhs }]
        when 'eventdataVal' then ['cond', "_event.data == #{value}"]
        when 'eventdataSomeVal' then ['cond', '_event.data == 123']
        when 'emptyEventData' then ['cond', "typeof _event.data === 'undefined'"]
        when 'nameVarVal' then ['cond', "_name  === '#{value}'"]
        when 'varPrefix' then ['cond', var_prefix(value)]
        when 'inState' then ['cond', "In('#{value}')"]
        when 'nonBoolean' then %w[cond return]
        when 'isBound' then ['cond', var(value)]
        when 'unboundVar' then ['cond', "typeof #{var(value)} === 'undefined' "]
        when 'systemVarIsBound' then ['cond', value]
        when 'noValue' then ['cond', "!#{var(value)}"]
        when 'true' then %w[cond true]
        when 'false' then %w[cond false]
        when 'eventFieldsAreBound'
          fields = %w[name type sendid origin origintype invokeid data]
          ['cond', fields.map { |f| "'#{f}' in _event" }.join(' && ')]
        when 'eventDataHasField' then ['cond', "#{value} in _event.data"]
        when 'eventFieldHasNoValue' then ['cond', "typeof _event.#{value} === 'undefined' "]
        when 'eventLanguageMatchesDatamodel' then ['cond', " _event.language == 'ecmascript'"]
        when 'originTypeEq' then ['cond', " _event.origintype == '#{value}'"]
        when 'arrayVar', 'arrayTextVar' then ['array', var(value)]
        when 'arrayExpr123' then ['expr', '1, 2, 3]']
        when 'multiDimensionalArrayExpr' then ['expr', "[[#{value},#{value}],[#{value},#{value}]]"]
        when 'arrayVal' then ['cond', array_val(value)]
        when 'illegalArray' then ['expr', '7']
        when 'item' then ['item', var(value)]
        when 'illegalItem' then ['item', "'continue'"]
        when 'index' then ['index', var(value)]
        when /\AbasicHTTP|\AmethodIs|\AeventIdParam|\AeventNamedParam|\AmessageBody|\AmsgIsBody|\AtestOnServer|
               \AeventIsExternal|\AmultipleNamelist|\AgetNamedParamVal|\AgetIDParamVal|\AgetNth|\AscxmlEventExpr/x
          raise Unsupported, "conf:#{name} needs the BasicHTTP Event I/O Processor"
        when 'scriptBadSrc', 'scriptGoodSrc'
          raise Unsupported, "conf:#{name} needs <script src>, which the importer would have to reject"
        else
          raise Unsupported, "conf:#{name} has no ECMAScript mapping"
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
        when 'incrementID' then [var(attributes['id']), "#{var(attributes['id'])} + 1"]
        when 'concatVars', 'sumVars'
          [var(attributes['id1']), "#{var(attributes['id1'])} + #{var(attributes['id2'])}"]
        when 'extendArray' then [var(attributes['id']), "[].concat(#{var(attributes['id'])}, [4])"]
        end
      end

      # The body of conf:script.
      def conf_script = 'var Var1 = 1'

      # conf:sendToSender's targetexpr and typeexpr.
      def sender_exprs = %w[_event.origin _event.origintype]

      # Expressions, locations, and scripts the document writes itself are
      # ECMAScript already.
      def plain_expression(source) = check_dom(source.to_s)

      def plain_location(source) = check_dom(source.to_s)

      # A namelist entry, used as a location; confEcma.xsl's invalid namelist
      # `"foo` fails when the data model evaluates it.
      def namelist_location(name) = name

      def script_body(text) = check_dom(text.to_s)

      def check_dom(source)
        if (match = DOM_METHODS.match(source))
          raise Unsupported, "needs an XML DOM in the ECMAScript data model (#{match[1]})"
        end

        source
      end

      # confEcma.xsl's comparison templates: Var<n>, the operator (with '='
      # replaced by `equals`), and the right-hand side.
      def comparison(value, equals, pattern: COMPARISON, left: ->(id) { var(id) })
        match = pattern.match(value) or raise Unsupported, "cannot parse comparison #{value.inspect}"
        operator = match[2] == '=' ? equals : match[2]
        "#{left.call(match[1])}#{operator}#{yield match[3]}"
      end

      def var_eq_var(value, operator)
        match = /\A([0-9]+)(\W+)([0-9]+)\z/.match(value) or raise Unsupported, "cannot parse #{value.inspect}"
        "#{var(match[1])}#{operator}#{var(match[3])}"
      end

      def var_prefix(value)
        match = /\A(\w+)(\W)(\w+)\z/.match(value) or raise Unsupported, "cannot parse #{value.inspect}"
        "#{STARTS_WITH}(#{var(match[3])}, #{var(match[1])})"
      end

      def array_val(value)
        match = /\A([0-9]+)(\W+)(.*)\z/.match(value) or raise Unsupported, "cannot parse #{value.inspect}"
        array = var(match[1])
        item = match[3]
        "#{array}[0][0]== #{item} && #{array}[0][1] == #{item}  && #{array}[1][0] == #{item} && #{array}[1][1] == #{item}"
      end
    end
  end
end
