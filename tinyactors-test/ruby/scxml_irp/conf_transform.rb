# frozen_string_literal: true

require 'json'
require 'rexml/document'
require_relative 'jq_expressions'
require_relative 'ecma_expressions'
require_relative 'null_expressions'

module ScxmlIrp
  SCXML_NS = 'http://www.w3.org/2005/07/scxml'
  CONF_NS = 'http://www.w3.org/2005/scxml-conformance'

  # Rewrites one IRP document (a .txml file) as an SCXML document for one data
  # model: the job vendor/scxml-irp/confEcma.xsl does for ECMAScript. The
  # result is plain SCXML, and reading it is the SCXML codec's job
  # (tinyactors-scxml, ADR 0053): the host imports it with
  # ta_definition_import. Nothing here interprets SCXML beyond what the
  # rewrite needs.
  #
  # The rewrite:
  #
  # - replaces `conf:` attributes and elements with what they stand for in
  #   the document's dialect (JqExpressions, EcmaExpressions,
  #   NullExpressions); a child machine without a datamodel uses its parent's;
  # - translates the small ECMAScript expressions the tests write directly
  #   into jq, attribute by attribute, for a jq document;
  # - turns a `namelist` into <param>s when the dialect writes a name's
  #   location differently from the name (jq writes `.Var1`);
  # - declares `_irp_outcome` in the test's own document and assigns
  #   "pass" or "fail" to it on entry to the pass and fail final states, so
  #   the scenario can read the outcome;
  # - names a document that has no `name`, since a chart's family is its name;
  # - rewrites inline child machines (<content><scxml>, and <scxml> as the
  #   value of <assign> or <data>) in place, with the same dialect.
  #
  # `<data src>` is kept: the scenario gives the file to the host's resolver.
  # A construct the dialect cannot express raises Unsupported, which skips
  # the test with that reason.
  class ConfTransform
    DIALECTS = { 'jq' => JqExpressions, 'ecmascript' => EcmaExpressions, 'null' => NullExpressions }.freeze

    # The data the test's own document declares to record its outcome.
    OUTCOME = '_irp_outcome'

    # Attributes whose plain value is an expression, or a location, per element.
    EXPRESSIONS = {
      'data' => %w[expr], 'transition' => %w[cond], 'log' => %w[expr], 'assign' => %w[expr],
      'param' => %w[expr], 'content' => %w[expr], 'send' => %w[eventexpr targetexpr typeexpr delayexpr],
      'invoke' => %w[typeexpr srcexpr], 'cancel' => %w[sendidexpr], 'if' => %w[cond], 'elseif' => %w[cond],
      'foreach' => %w[array]
    }.freeze
    LOCATIONS = {
      'assign' => %w[location], 'param' => %w[location], 'send' => %w[idlocation], 'invoke' => %w[idlocation]
    }.freeze

    # Parents whose children are executable content, literal content, or a
    # payload (<param>, <content>).
    ACTION_PARENTS = %w[onentry onexit transition if foreach finalize].freeze
    CONTENT_PARENTS = %w[content data assign].freeze
    PAYLOAD_PARENTS = %w[send invoke donedata].freeze

    Result = Data.define(:source, :datamodel, :files)

    def self.parse(path)
      REXML::Document.new(File.read(path)).root
    end

    # `top_level:` marks the test's own document, which records its outcome.
    def initialize(root, chart_name:, top_level:, resolve_file:, dialect: JqExpressions)
      @root = root
      @dialect = dialect
      @chart_name = chart_name
      @top_level = top_level
      @resolve_file = resolve_file
      @files = {}
      @children = 0
      @out = +''
    end

    def convert
      raise Unsupported, "root element is <#{@root.name}>, not <scxml>" unless scxml?(@root, 'scxml')

      @dialect = dialect_for(plain_attribute(@root, 'datamodel'))
      element(@root, parent: nil)
      Result.new(source: "#{@out}\n", datamodel: @dialect.datamodel, files: @files)
    end

    private

    def dialect_for(datamodel)
      return @dialect if datamodel.nil?

      DIALECTS.fetch(datamodel) do
        raise Unsupported, "requires the #{datamodel} data model (the harness provides jq, ecmascript and null)"
      end
    end

    # An unprefixed attribute's value. (REXML's `attributes[name]` also finds conf:name.)
    def plain_attribute(element, name)
      found = nil
      element.attributes.each_attribute do |attribute|
        found = attribute.value if attribute.name == name && attribute.prefix.to_s.empty?
      end
      found
    end

    def scxml?(element, name = nil)
      element.namespace == SCXML_NS && (name.nil? || element.name == name)
    end

    def conf?(element) = element.namespace == CONF_NS

    def elements(element) = element.elements.to_a

    # ---- writing ----

    def text(value)
      value.to_s.gsub('&', '&amp;').gsub('<', '&lt;').gsub('>', '&gt;')
    end

    def attribute_text(value)
      text(value).gsub('"', '&quot;').gsub("\t", '&#9;').gsub("\n", '&#10;').gsub("\r", '&#13;')
    end

    def open_tag(name, attributes, empty: false)
      pairs = attributes.map { |key, value| %( #{key}="#{attribute_text(value)}") }.join
      @out << "<#{name}#{pairs}#{empty ? '/>' : '>'}"
    end

    def close_tag(name) = @out << "</#{name}>"

    def leaf(name, attributes) = open_tag(name, attributes, empty: true)

    # ---- attributes ----

    # The element's SCXML attributes after the conf: mapping, in document
    # order, as { name => value }; plain expressions and locations are
    # translated for the dialect.
    def attributes(element)
      result = {}
      element.attributes.each_attribute do |attribute|
        next if attribute.prefix == 'xmlns' || attribute.name == 'xmlns'

        if attribute.namespace == CONF_NS
          name, value = @dialect.conf_attribute(attribute.name, attribute.value)
        elsif attribute.prefix.to_s.empty?
          name = attribute.name
          value = plain(element.name, name, attribute.value)
        else
          raise Unsupported, "attribute #{attribute.expanded_name} on <#{element.name}>"
        end
        raise Unsupported, "<#{element.name}> sets #{name} twice" if result.key?(name)

        result[name] = value
      end
      result
    end

    def plain(element, name, value)
      if EXPRESSIONS.fetch(element, []).include?(name)
        @dialect.plain_expression(value)
      elsif LOCATIONS.fetch(element, []).include?(name)
        @dialect.plain_location(value)
      else
        value
      end
    end

    # ---- elements ----

    def element(node, parent:)
      return conf_element(node, parent) if conf?(node)
      raise Unsupported, "<#{node.name}> from namespace #{node.namespace}" unless scxml?(node)

      attrs = attributes(node)
      children = elements(node)
      params = namelist_params(node, attrs)
      check(node, attrs, children)
      outcome = written_outcome(node, attrs, parent)

      if node.equal?(@root) || (node.name == 'scxml' && parent)
        attrs['name'] ||= @chart_name
        attrs = { 'xmlns' => SCXML_NS }.merge(attrs)
      end
      if node.name == 'script' && parent
        return script(node, attrs)
      end

      open_tag(node.name, attrs)
      params.each { |name, location| leaf('param', { 'name' => name, 'location' => location }) }
      if @top_level && node.equal?(@root) && children.none? { |child| scxml?(child, 'datamodel') }
        outcome_datamodel
      end
      if outcome && children.none? { |child| scxml?(child, 'onentry') }
        open_tag('onentry', {})
        record_outcome(outcome)
        close_tag('onentry')
      end
      body(node)
      close_tag(node.name)
    end

    # Text, CDATA and elements of `node`, in order; comments are dropped.
    def body(node)
      first_datamodel = @top_level && node.equal?(@root)
      node.children.each do |child|
        case child
        when REXML::CData then @out << "<![CDATA[#{child.value}]]>"
        when REXML::Text then @out << text(child.value)
        when REXML::Element
          if CONTENT_PARENTS.include?(node.name) && scxml?(child, 'scxml')
            inline_machine(node, child)
          elsif CONTENT_PARENTS.include?(node.name) && !conf?(child)
            # Markup that is literal content is data, written as it was.
            @out << child.to_s
          elsif first_datamodel && scxml?(child, 'datamodel')
            first_datamodel = false
            datamodel_with_outcome(child)
          elsif scxml?(child, 'onentry') && (outcome = written_outcome(node, attributes(node), node.parent))
            open_tag('onentry', {})
            body(child)
            record_outcome(outcome)
            close_tag('onentry')
          else
            element(child, parent: node)
          end
        end
      end
    end

    # ---- the outcome ----

    def outcome_datamodel
      open_tag('datamodel', {})
      leaf('data', { 'id' => OUTCOME })
      close_tag('datamodel')
    end

    def datamodel_with_outcome(node)
      open_tag('datamodel', attributes(node))
      leaf('data', { 'id' => OUTCOME })
      body(node)
      close_tag('datamodel')
    end

    # A top-level <final id="pass"> or <final id="fail"> that the test writes
    # itself instead of conf:pass or conf:fail: its outcome, or nil.
    def written_outcome(node, attrs, parent)
      return nil unless @top_level && node.name == 'final' && parent.equal?(@root)

      attrs['id'] if %w[pass fail].include?(attrs['id'])
    end

    # Logs the outcome, and records it in the test's own document. A data
    # model that records nothing (the null profile) cannot record it.
    def record_outcome(outcome)
      if records_outcome?
        leaf('log', { 'label' => 'Outcome', 'expr' => @dialect.string(outcome) })
      else
        leaf('log', { 'label' => "Outcome.#{outcome}" })
      end
      return unless @top_level
      raise Unsupported, 'the null data model cannot record a test outcome; it needs a log sink' unless records_outcome?

      leaf('assign', { 'location' => @dialect.data_location(OUTCOME), 'expr' => @dialect.string(outcome) })
    end

    def records_outcome?
      !@dialect.respond_to?(:records_outcome?) || @dialect.records_outcome?
    end

    # ---- conf: elements ----

    def conf_element(node, parent)
      parent_name = parent&.name
      if %w[pass fail].include?(node.name) && %w[scxml state parallel].include?(parent_name)
        open_tag('final', { 'id' => node.name })
        open_tag('onentry', {})
        record_outcome(node.name)
        close_tag('onentry')
        close_tag('final')
      elsif node.name == 'script' && (parent_name == 'scxml' || ACTION_PARENTS.include?(parent_name))
        open_tag('script', {})
        @out << text(@dialect.conf_script)
        close_tag('script')
      elsif ACTION_PARENTS.include?(parent_name)
        conf_action(node)
      elsif CONTENT_PARENTS.include?(parent_name)
        @out << text(@dialect.conf_content(node.name))
      elsif PAYLOAD_PARENTS.include?(parent_name) && node.name == 'contentFoo'
        open_tag('content', {})
        @out << text(@dialect.conf_content(node.name))
        close_tag('content')
      else
        raise Unsupported, "conf:#{node.name} in <#{parent_name}>"
      end
    end

    def conf_action(node)
      case node.name
      when 'incrementID', 'concatVars', 'sumVars', 'extendArray'
        location, expr = @dialect.conf_assignment(node.name, node.attributes)
        leaf('assign', { 'location' => location, 'expr' => expr })
      when 'sendToSender'
        to, type = @dialect.sender_exprs
        leaf('send', { 'event' => node.attributes['name'], 'targetexpr' => to, 'typeexpr' => type })
      else
        raise Unsupported, "conf:#{node.name} as executable content"
      end
    end

    # ---- what the rewrite checks and changes ----

    # The constructs the importer has always refused, so a test's skip
    # decision does not depend on the codec.
    def check(node, attrs, children)
      case node.name
      when 'send'
        raise Unsupported, "<send> with conf-mapped expr #{attrs['expr']}" if attrs.key?('expr')

        duration(attrs['delay']) if attrs.key?('delay')
      when 'content'
        raise Unsupported, '<content> with both expr and a body' if attrs.key?('expr') && body?(node)
      when 'assign'
        raise Unsupported, '<assign> with both expr and content' if attrs.key?('expr') && body?(node)
      when 'data'
        initializers = [attrs['expr'], attrs['src'], body?(node) ? true : nil].compact
        raise Unsupported, "<data id=#{attrs['id']}> has more than one initializer" if initializers.size > 1

        @files[attrs['src']] = @resolve_file.call(attrs['src']) if attrs.key?('src')
      when 'script'
        raise Unsupported, '<script src>' if attrs.key?('src')
      end
      children
    end

    def body?(node)
      node.children.any? { |child| child.is_a?(REXML::Element) || (child.is_a?(REXML::Text) && !child.value.strip.empty?) }
    end

    # SCXML durations must be whole milliseconds, which is all the core keeps.
    def duration(text)
      match = /\A(\d*\.?\d+)(ms|s)\z/.match(text.to_s.strip) or raise Unsupported, "delay #{text.inspect}"
      milliseconds = match[1].to_f * (match[2] == 's' ? 1000 : 1)
      raise Unsupported, "delay #{text.inspect} is not whole milliseconds" unless milliseconds == milliseconds.round
    end

    # A namelist whose names the dialect writes as different locations
    # becomes <param>s, which is what SCXML makes of a namelist anyway.
    def namelist_params(node, attrs)
      return [] unless %w[send invoke].include?(node.name) && attrs.key?('namelist')

      names = attrs['namelist'].split
      locations = names.map { |name| @dialect.namelist_location(name) }
      return [] if locations == names

      attrs.delete('namelist')
      names.zip(locations)
    end

    # A <script> in executable content: its body in the dialect.
    def script(node, attrs)
      body = node.children.grep(REXML::Text).map(&:value).join
      open_tag('script', attrs)
      @out << text(@dialect.script_body(body))
      close_tag('script')
    end

    # An inline <scxml> document: a child machine, rewritten in place with
    # this document's dialect.
    def inline_machine(parent, machine)
      text = parent.children.grep(REXML::Text).map(&:value).join
      unless elements(parent).size == 1 && text.strip.empty?
        raise Unsupported, "<#{parent.name}> mixes an inline <scxml> document with other content"
      end

      @children += 1
      child = ConfTransform.new(machine, chart_name: "#{@chart_name}_child#{@children}", top_level: false,
                                         resolve_file: @resolve_file, dialect: @dialect)
      result = child.convert
      @files.merge!(result.files)
      @out << result.source.chomp
    end
  end
end
