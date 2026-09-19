# frozen_string_literal: true

require 'json'
require 'rexml/document'
require_relative 'jq_expressions'

module ScxmlIrp
  SCXML_NS = 'http://www.w3.org/2005/07/scxml'
  CONF_NS = 'http://www.w3.org/2005/scxml-conformance'

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

    # One symbol, or an array of symbols.
    def targets(list)
      list.size == 1 ? sym(list.first) : "[#{list.map { |t| sym(t) }.join(', ')}]"
    end

    def hash_key(text)
      text = text.to_s
      text.match?(/\A[A-Za-z_]\w*\z/) ? "#{text}:" : "#{text.inspect} =>"
    end

    def kwargs(pairs)
      pairs.compact.map { |key, value| "#{key}: #{value}" }.join(', ')
    end
  end

  # Converts one SCXML IRP document (a .txml file) into chart DSL source for
  # the jq data model. Inline child machines of <invoke><content> become
  # charts of their own, emitted before the chart that invokes them.
  #
  # Every element and attribute must be understood; anything else raises
  # Unsupported rather than being dropped silently.
  class Document
    include JqExpressions

    Result = Data.define(:chart_name, :source, :notes)

    IGNORED_ATTRIBUTES = %w[version].freeze

    attr_reader :notes

    # The data the test's own document declares to record its outcome. The
    # conf:pass and conf:fail final states assign "pass" or "fail" to it on
    # entry, and the scenario reads it from the finished actor's data model.
    # (SCXML would report the outcome as <donedata>; the data model works
    # before the core compiles <donedata>.)
    OUTCOME = '_irp_outcome'

    # `top_level:` marks the test's own document, which records its outcome.
    def initialize(root, chart_name:, top_level:, resolve_file:)
      @root = root
      @chart_name = chart_name
      @top_level = top_level
      @resolve_file = resolve_file
      @notes = []
      @children = []
      @anonymous = 0
      @send_ids = []
    end

    def self.parse(path)
      REXML::Document.new(File.read(path)).root
    end

    # Returns the Ruby source of every chart this document needs, children
    # first.
    def convert
      raise Unsupported, "root element is <#{@root.name}>, not <scxml>" unless scxml?(@root, 'scxml')

      @send_ids = literal_send_ids(@root)
      emitter = Emitter.new
      emit_chart(emitter)
      (@children.map(&:source) + [emitter.to_s]).join("\n")
    end

    def child_notes = @children.flat_map(&:notes)

    # Literal <send id> values of this document, excluding inline child
    # machines, which become charts of their own.
    def literal_send_ids(element)
      element.elements.to_a.flat_map do |child|
        next [] if scxml?(child, 'content')

        own = scxml?(child, 'send') && child.attributes['id'] ? [child.attributes['id']] : []
        own + literal_send_ids(child)
      end
    end

    private

    def scxml?(element, name = nil)
      element.namespace == SCXML_NS && (name.nil? || element.name == name)
    end

    def conf?(element) = element.namespace == CONF_NS

    def elements(element) = element.elements.to_a

    # The SCXML attributes of an element after the conf: mapping, as
    # { name => [value, :jq | :text] }. Expression values of plain SCXML
    # attributes are translated from ECMAScript later, by attribute kind.
    def attributes(element)
      result = {}
      element.attributes.each_attribute do |attribute|
        next if attribute.prefix == 'xmlns' || attribute.name == 'xmlns'
        next if IGNORED_ATTRIBUTES.include?(attribute.name) && attribute.prefix.to_s.empty?

        if attribute.namespace == CONF_NS
          name, value = conf_attribute(attribute.name, attribute.value)
          add_attribute(result, element, name, [value, :conf])
        elsif attribute.prefix.to_s.empty?
          add_attribute(result, element, attribute.name, [attribute.value, :plain])
        else
          raise Unsupported, "attribute #{attribute.expanded_name} on <#{element.name}>"
        end
      end
      result
    end

    def add_attribute(result, element, name, value)
      raise Unsupported, "<#{element.name}> sets #{name} twice" if result.key?(name)

      result[name] = value
    end

    def check_attributes!(element, attrs, allowed)
      unknown = attrs.keys - allowed
      raise Unsupported, "<#{element.name}> attribute(s) #{unknown.join(', ')}" unless unknown.empty?
    end

    # jq source of an expression attribute.
    def expression(attrs, name)
      value, origin = attrs[name]
      return nil if value.nil?

      origin == :conf ? value : ecmascript(value)
    end

    def location_attribute(attrs, name)
      value, origin = attrs[name]
      return nil if value.nil?

      origin == :conf ? value : ecmascript_location(value)
    end

    def text(attrs, name) = attrs[name]&.first

    def id_list(attrs, name) = text(attrs, name)&.split

    def emit_chart(emitter)
      attrs = attributes(@root)
      check_attributes!(@root, attrs, %w[initial name datamodel binding xmlns])
      datamodel = text(attrs, 'datamodel')
      if datamodel && datamodel != 'jq'
        raise Unsupported, "requires the #{datamodel} data model (the harness provides jq)"
      end

      options = { datamodel: datamodel ? nil : 'nil' }
      options[:binding] = Ruby.sym(text(attrs, 'binding')) if text(attrs, 'binding')
      options[:document_name] = Ruby.str(text(attrs, 'name')) if text(attrs, 'name')
      header = "TinyactorsTest.chart #{Ruby.sym(@chart_name)}"
      header += ", #{Ruby.kwargs(options)}" unless options.compact.empty?
      emitter.block(header) do
        emitter.line("initial #{id_list(attrs, 'initial').map { |t| Ruby.sym(t) }.join(', ')}") if attrs['initial']
        emitter.line("data #{Ruby.sym(OUTCOME)}") if @top_level
        elements(@root).each do |child|
          if conf?(child) && child.name == 'script'
            emitter.line("script #{Ruby.str('.Var1 = 1')}")
            next
          elsif conf?(child)
            emit_conf_state(emitter, child)
            next
          end
          case child.name
          when 'datamodel' then emit_datamodel(emitter, child)
          when 'script'
            emitter.line("script #{Ruby.str(script_source(child))}")
          when 'state', 'parallel', 'final' then emit_state(emitter, child)
          else raise Unsupported, "<#{child.name}> in <scxml>"
          end
        end
      end
    end

    def emit_conf_state(emitter, element)
      outcome = case element.name
                when 'pass' then 'pass'
                when 'fail' then 'fail'
                else raise Unsupported, "conf:#{element.name} as a state"
                end
      emitter.block("final :#{outcome}") do
        emitter.block('on_entry') do
          emitter.line("log 'Outcome', expr: #{Ruby.str(string(outcome))}")
          emitter.line("assign #{Ruby.str(".#{OUTCOME}")}, #{Ruby.str(string(outcome))}") if @top_level
        end
      end
    end

    def emit_datamodel(emitter, element)
      check_attributes!(element, attributes(element), [])
      elements(element).each do |data|
        raise Unsupported, "<#{data.name}> in <datamodel>" unless scxml?(data, 'data')

        emit_data(emitter, data)
      end
    end

    def emit_data(emitter, data)
      attrs = attributes(data)
      check_attributes!(data, attrs, %w[id expr src])
      name = text(attrs, 'id') or raise Unsupported, '<data> without id'
      initializers = [attrs['expr'], attrs['src'], inline_text(data)].compact
      raise Unsupported, "<data id=#{name}> has more than one initializer" if initializers.size > 1

      value =
        if attrs['expr']
          "{ expr: #{Ruby.str(expression(attrs, 'expr'))} }"
        elsif attrs['src']
          "{ content: #{Ruby.str(@resolve_file.call(text(attrs, 'src')))} }"
        elsif (machine = inline_machine(data))
          "{ chart: #{Ruby.sym(child_chart(machine))} }"
        elsif (content = inline_text(data))
          "{ content: #{Ruby.str(content)} }"
        end
      emitter.line(value ? "data #{Ruby.hash_key(name)} #{value}" : "data #{Ruby.sym(name)}")
    end

    # The inline <scxml> document that is an element's whole content, or nil
    # when it has none. The importer converts it to a child chart, whose spec
    # is the value of the document: the SCXML invoker runs that spec, and the
    # host has no XML parser for the document text (ADR 0025).
    def inline_machine(element)
      machine = elements(element).find { |child| scxml?(child, 'scxml') }
      return nil unless machine

      text = element.children.grep(REXML::Text).map(&:value).join
      unless elements(element).size == 1 && text.strip.empty?
        raise Unsupported, "<#{element.name}> mixes an inline <scxml> document with other content"
      end

      machine
    end

    # The text of an element's inline content, with conf: content elements
    # replaced; nil when it has none. Child XML elements are serialized.
    def inline_text(element)
      parts = element.children.map do |node|
        case node
        when REXML::Text then node.value
        when REXML::Comment then ''
        when REXML::Element
          conf?(node) ? conf_content(node.name) : node.to_s
        else ''
        end
      end
      text = parts.join
      text.strip.empty? ? nil : text
    end

    def emit_state(emitter, element)
      attrs = attributes(element)
      kind = element.name
      allowed = kind == 'state' ? %w[id initial] : %w[id]
      check_attributes!(element, attrs, allowed)
      id = text(attrs, 'id') || anonymous_id
      header = "#{kind} #{Ruby.sym(id)}"
      header += ", initial: #{Ruby.targets(id_list(attrs, 'initial'))}" if attrs['initial']

      children = elements(element)
      if children.empty?
        emitter.line(header)
        return
      end

      emitter.block(header) do
        children.each do |child|
          if conf?(child)
            emit_conf_state(emitter, child)
            next
          end
          raise Unsupported, "<#{child.name}> from namespace #{child.namespace}" unless scxml?(child)

          case child.name
          when 'state', 'parallel', 'final' then emit_state(emitter, child)
          when 'history' then emit_history(emitter, child)
          when 'initial' then emit_initial(emitter, child)
          when 'transition' then emit_transition(emitter, child)
          when 'onentry' then emit_actions_block(emitter, 'on_entry', child)
          when 'onexit' then emit_actions_block(emitter, 'on_exit', child)
          when 'datamodel' then emit_datamodel(emitter, child)
          when 'invoke' then emit_invoke(emitter, child)
          when 'donedata'
            raise Unsupported, '<donedata> outside <final>' unless kind == 'final'

            emit_donedata(emitter, child)
          else raise Unsupported, "<#{child.name}> in <#{kind}>"
          end
        end
      end
    end

    def anonymous_id
      @anonymous += 1
      "irp_anonymous_#{@anonymous}"
    end

    def emit_history(emitter, element)
      attrs = attributes(element)
      check_attributes!(element, attrs, %w[id type])
      transitions = elements(element)
      unless transitions.size == 1 && scxml?(transitions.first, 'transition')
        raise Unsupported, '<history> without exactly one default <transition>'
      end

      transition = transitions.first
      targets = transition_targets(transition)
      type = text(attrs, 'type') || 'shallow'
      header = "history #{Ruby.sym(text(attrs, 'id'))}, type: :#{type}, default: #{Ruby.targets(targets)}"
      emit_with_actions(emitter, header, elements(transition))
    end

    def emit_initial(emitter, element)
      transitions = elements(element)
      unless transitions.size == 1 && scxml?(transitions.first, 'transition')
        raise Unsupported, '<initial> without exactly one <transition>'
      end

      transition = transitions.first
      header = "initial #{transition_targets(transition).map { |t| Ruby.sym(t) }.join(', ')}"
      emit_with_actions(emitter, header, elements(transition))
    end

    def transition_targets(transition)
      attrs = attributes(transition)
      check_attributes!(transition, attrs, %w[target])
      id_list(attrs, 'target') or raise Unsupported, 'default transition without target'
    end

    def emit_transition(emitter, element)
      attrs = attributes(element)
      check_attributes!(element, attrs, %w[event cond target type])
      options = []
      options << "on: #{Ruby.str(text(attrs, 'event'))}" if attrs['event']
      options << "target: #{Ruby.targets(id_list(attrs, 'target'))}" if attrs['target']
      options << "cond: #{Ruby.str(expression(attrs, 'cond'))}" if attrs['cond']
      options << "type: :#{text(attrs, 'type')}" if attrs['type']
      emit_with_actions(emitter, "transition #{options.join(', ')}".strip, elements(element))
    end

    def emit_with_actions(emitter, header, actions)
      if actions.empty?
        emitter.line(header)
      else
        emitter.block(header) { emit_actions(emitter, actions) }
      end
    end

    def emit_actions_block(emitter, name, element)
      check_attributes!(element, attributes(element), [])
      emitter.block(name) { emit_actions(emitter, elements(element)) }
    end

    def emit_actions(emitter, actions)
      actions.each { |action| emit_action(emitter, action) }
    end

    def emit_action(emitter, element)
      return emit_conf_action(emitter, element) if conf?(element)
      raise Unsupported, "<#{element.name}> from namespace #{element.namespace}" unless scxml?(element)

      attrs = attributes(element)
      case element.name
      when 'raise'
        check_attributes!(element, attrs, %w[event])
        emitter.line("raise_event #{Ruby.str(text(attrs, 'event'))}")
      when 'log'
        check_attributes!(element, attrs, %w[label expr])
        options = []
        options << Ruby.str(text(attrs, 'label')) if attrs['label']
        options << "expr: #{Ruby.str(expression(attrs, 'expr'))}" if attrs['expr']
        emitter.line("log #{options.join(', ')}".strip)
      when 'assign' then emit_assign(emitter, element, attrs)
      when 'send' then emit_send(emitter, element, attrs)
      when 'cancel'
        check_attributes!(element, attrs, %w[sendid sendidexpr])
        if attrs['sendid']
          sendid = text(attrs, 'sendid')
          unmatched = @send_ids.include?(sendid) ? '' : ', unmatched: true'
          emitter.line("cancel #{Ruby.str(sendid)}#{unmatched}")
        else
          emitter.line("cancel expr: #{Ruby.str(expression(attrs, 'sendidexpr'))}")
        end
      when 'if' then emit_if(emitter, element, attrs)
      when 'foreach' then emit_foreach(emitter, element, attrs)
      when 'script'
        check_attributes!(element, attrs, [])
        emitter.line("script #{Ruby.str(script_source(element))}")
      else
        raise Unsupported, "executable content <#{element.name}>"
      end
    end

    def emit_conf_action(emitter, element)
      id = element.attributes['id']
      case element.name
      when 'incrementID'
        emitter.line("assign #{Ruby.str(location(id))}, #{Ruby.str("#{location(id)} + 1")}")
      when 'concatVars', 'sumVars'
        first = element.attributes['id1']
        second = element.attributes['id2']
        emitter.line("assign #{Ruby.str(location(first))}, #{Ruby.str("#{location(first)} + #{location(second)}")}")
      when 'extendArray'
        emitter.line("assign #{Ruby.str(location(id))}, #{Ruby.str("#{location(id)} + [4]")}")
      when 'script'
        emitter.line("script #{Ruby.str('.Var1 = 1')}")
      when 'sendToSender'
        name = element.attributes['name']
        emitter.line("send_event #{Ruby.str(name)}, to_expr: '$_event.origin', type_expr: '$_event.origintype'")
      else
        raise Unsupported, "conf:#{element.name} as executable content"
      end
    end

    # A conf:script or plain <script> body. The IRP only uses conf:script for
    # data-model-neutral tests; plain scripts are ECMAScript.
    def script_source(element)
      attrs = attributes(element)
      raise Unsupported, '<script src>' if attrs['src']

      raise Unsupported, "ECMAScript <script> #{inline_text(element).to_s.strip.inspect}"
    end

    def emit_assign(emitter, element, attrs)
      check_attributes!(element, attrs, %w[location expr])
      target = location_attribute(attrs, 'location') or raise Unsupported, '<assign> without location'
      if attrs['expr']
        raise Unsupported, '<assign> with both expr and content' if inline_text(element)

        emitter.line("assign #{Ruby.str(target)}, #{Ruby.str(expression(attrs, 'expr'))}")
      elsif (machine = inline_machine(element))
        emitter.line("assign #{Ruby.str(target)}, chart: #{Ruby.sym(child_chart(machine))}")
      else
        content = inline_text(element) or raise Unsupported, '<assign> without a value'
        emitter.line("assign #{Ruby.str(target)}, content: #{Ruby.str(content)}")
      end
    end

    def emit_send(emitter, element, attrs)
      check_attributes!(element, attrs, %w[event eventexpr target targetexpr type typeexpr id idlocation
                                           delay delayexpr namelist expr])
      raise Unsupported, "<send> with conf-mapped expr #{text(attrs, 'expr')}" if attrs['expr']

      arguments = []
      arguments << Ruby.str(text(attrs, 'event')) if attrs['event']
      options = {}
      options[:event_expr] = Ruby.str(expression(attrs, 'eventexpr')) if attrs['eventexpr']
      options[:to] = Ruby.str(text(attrs, 'target')) if attrs['target']
      options[:to_expr] = Ruby.str(expression(attrs, 'targetexpr')) if attrs['targetexpr']
      options[:type] = Ruby.str(text(attrs, 'type')) if attrs['type']
      options[:type_expr] = Ruby.str(expression(attrs, 'typeexpr')) if attrs['typeexpr']
      if attrs['id']
        options[:id] = Ruby.str(text(attrs, 'id'))
      end
      options[:id_location] = Ruby.str(location_attribute(attrs, 'idlocation')) if attrs['idlocation']
      options[:delay] = duration(text(attrs, 'delay')).to_s if attrs['delay']
      options[:delay_expr] = Ruby.str(expression(attrs, 'delayexpr')) if attrs['delayexpr']

      params, content = payload(element, attrs)
      options[:params] = params if params
      options.merge!(content) if content
      emitter.line("send_event #{[*arguments, Ruby.kwargs(options)].reject(&:empty?).join(', ')}".strip)
    end

    # SCXML durations as integer milliseconds.
    def duration(text)
      match = /\A(\d*\.?\d+)(ms|s)\z/.match(text.to_s.strip) or raise Unsupported, "delay #{text.inspect}"
      milliseconds = match[1].to_f * (match[2] == 's' ? 1000 : 1)
      raise Unsupported, "delay #{text.inspect} is not whole milliseconds" unless milliseconds == milliseconds.round

      milliseconds.round
    end

    # The namelist, <param>, and <content> of a <send>, <invoke>, or
    # <donedata>: [params source or nil, { content: or content_expr: } or nil].
    def payload(element, attrs, children = elements(element), content_key: :content)
      params = []
      id_list(attrs, 'namelist')&.each do |name|
        params << [name, "{ location: #{Ruby.str(name.start_with?('"') ? error('invalidNamelist') : ecmascript_location(name))} }"]
      end
      content = nil
      children.each do |child|
        if conf?(child)
          raise Unsupported, "conf:#{child.name} in <#{element.name}>" unless child.name == 'contentFoo'

          content = { content_key => Ruby.str(conf_content(child.name)) }
          next
        end
        case child.name
        when 'param'
          param_attrs = attributes(child)
          check_attributes!(child, param_attrs, %w[name expr location])
          name = text(param_attrs, 'name') or raise Unsupported, '<param> without name'
          value =
            if param_attrs['expr']
              Ruby.str(expression(param_attrs, 'expr'))
            elsif param_attrs['location']
              "{ location: #{Ruby.str(location_attribute(param_attrs, 'location'))} }"
            else
              raise Unsupported, "<param name=#{name}> without expr or location"
            end
          params << [name, value]
        when 'content'
          raise Unsupported, "<#{element.name}> with more than one <content>" if content

          content = content_value(child, content_key)
        when 'finalize'
          next
        else
          raise Unsupported, "<#{child.name}> in <#{element.name}>"
        end
      end
      raise Unsupported, "<#{element.name}> with both <content> and params" if content && !params.empty?

      params_source = params.empty? ? nil : "{ #{params.map { |name, value| "#{Ruby.hash_key(name)} #{value}" }.join(', ')} }"
      [params_source, content]
    end

    def content_value(element, content_key)
      attrs = attributes(element)
      check_attributes!(element, attrs, %w[expr])
      if attrs['expr']
        raise Unsupported, '<content> with both expr and a body' if inline_text(element)

        { :"#{content_key}_expr" => Ruby.str(expression(attrs, 'expr')) }
      elsif (machine = inline_machine(element))
        { chart: Ruby.sym(child_chart(machine)) }
      else
        { content_key => Ruby.str(inline_text(element) || '') }
      end
    end

    def emit_if(emitter, element, attrs)
      check_attributes!(element, attrs, %w[cond])
      branches = [[expression(attrs, 'cond'), []]]
      otherwise = nil
      elements(element).each do |child|
        if scxml?(child, 'elseif')
          raise Unsupported, '<elseif> after <else>' if otherwise

          branch_attrs = attributes(child)
          check_attributes!(child, branch_attrs, %w[cond])
          branches << [expression(branch_attrs, 'cond'), []]
        elsif scxml?(child, 'else')
          raise Unsupported, 'two <else> in one <if>' if otherwise

          check_attributes!(child, attributes(child), [])
          otherwise = []
        else
          (otherwise || branches.last[1]) << child
        end
      end

      emitter.block('conditional') do
        branches.each do |condition, body|
          emit_with_actions(emitter, "branch #{Ruby.str(condition)}", body)
        end
        emit_with_actions(emitter, 'otherwise', otherwise) if otherwise
      end
    end

    def emit_foreach(emitter, element, attrs)
      check_attributes!(element, attrs, %w[array item index])
      array = expression(attrs, 'array') or raise Unsupported, '<foreach> without array'
      options = { item: Ruby.str(text(attrs, 'item')) }
      options[:index] = Ruby.str(text(attrs, 'index')) if attrs['index']
      emit_with_actions(emitter, "foreach #{Ruby.str(array)}, #{Ruby.kwargs(options)}", elements(element))
    end

    def emit_invoke(emitter, element)
      attrs = attributes(element)
      check_attributes!(element, attrs, %w[type typeexpr src srcexpr id idlocation namelist autoforward])
      options = {}
      options[:type] = Ruby.str(text(attrs, 'type')) if attrs['type']
      options[:type_expr] = Ruby.str(expression(attrs, 'typeexpr')) if attrs['typeexpr']
      options[:src] = Ruby.str(text(attrs, 'src')) if attrs['src']
      options[:src_expr] = Ruby.str(expression(attrs, 'srcexpr')) if attrs['srcexpr']
      options[:id] = Ruby.str(text(attrs, 'id')) if attrs['id']
      options[:id_location] = Ruby.str(location_attribute(attrs, 'idlocation')) if attrs['idlocation']
      options[:autoforward] = 'true' if text(attrs, 'autoforward') == 'true'

      inline = elements(element).find { |child| scxml?(child, 'content') }
      machine = inline && elements(inline).find { |child| scxml?(child, 'scxml') }
      if machine
        raise Unsupported, 'invoke <content> mixes a machine with other content' if elements(inline).size > 1

        options[:chart] = Ruby.sym(child_chart(machine))
        others = elements(element).reject { |child| child.equal?(inline) }
        params, = payload(element, attrs, others)
      else
        params, content = payload(element, attrs)
        options.merge!(content) if content
      end
      options[:params] = params if params

      finalize = elements(element).find { |child| scxml?(child, 'finalize') }
      header = "invoke #{Ruby.kwargs(options)}"
      if finalize
        emit_with_actions(emitter, header, elements(finalize))
      else
        emitter.line(header)
      end
    end

    def child_chart(machine)
      name = "#{@chart_name}_child#{@children.size + 1}"
      child = Document.new(machine, chart_name: name, top_level: false, resolve_file: @resolve_file)
      source = child.convert
      @children << Result.new(chart_name: name, source: source, notes: child.notes)
      name
    end

    def emit_donedata(emitter, element)
      attrs = attributes(element)
      check_attributes!(element, attrs, [])
      params, content = payload(element, attrs)
      options = {}
      options[:params] = params if params
      if content
        options[:content] = content[:content] if content[:content]
        options[:chart] = content[:chart] if content[:chart]
        options[:expr] = content[:content_expr] if content[:content_expr]
      end
      emitter.line("done_data #{Ruby.kwargs(options)}")
    end
  end
end
