# frozen_string_literal: true

module DAP
  # Modules and loaded sources. A module is one registered definition
  # revision: its name is the definition family, its version the revision,
  # and a revision the workbench retired after a reload stays listed, marked
  # retired. A loaded source is the chart file a revision was compiled
  # from, with the text it had then, so a retired revision still shows the
  # chart it ran.
  #
  # A definition compiled without source spans (a chart an `invoke chart:`
  # inlines: the DSL builds inline content without them) has no file to
  # point into. It is shown as a virtual source instead: the SCXML the codec
  # encodes from the definition, with the place the encode wrote each
  # element (core ADR 0086, `ta_definition_encode` with its source map),
  # served by `sourceReference`.
  class Catalog
    CODEC = { '$adapter' => 'codec.scxml' }.freeze
    SCXML = 'application/scxml+xml'
    RUBY = 'text/x-ruby'

    # An encoded chart: its text, and where each element is in it, as one
    # array of spans per kind, indexed by ID.
    Virtual = Struct.new(:reference, :name, :text, :spans, keyword_init: true)

    Entry = Struct.new(:definition, :family, :revision, :chart, :path, :text, :reference, :retired, :virtual,
                       keyword_init: true) do
      def module_body
        body = { 'id' => definition.id.to_s, 'name' => retired ? "#{family} (retired)" : family, 'version' => revision }
        body['path'] = path if path
        body
      end

      def source_body
        name = virtual ? virtual.name : "#{chart}.rb"
        body = { 'name' => "#{name}@#{revision.to_s[0, 8]}", 'sourceReference' => reference,
                 'presentationHint' => retired ? 'deemphasize' : 'normal' }
        body['name'] = "#{File.basename(path)}@#{revision.to_s[0, 8]}" if path
        body['path'] = path if path
        body
      end
    end

    def initialize(session, labels)
      @session = session
      @labels = labels
      @entries = {}
      @virtuals = {}
      @next_reference = 0
    end

    # Where the encode of a definition without spans wrote an element:
    # {reference:, name:, line:, column:, end_line:, end_column:}, or nil.
    def location(definition, kind, id)
      virtual = virtual(definition)
      span = virtual&.spans&.dig(:"#{kind}s", Integer(id))
      return nil if span.nil? || span[:line].to_i.zero?

      { reference: virtual.reference, name: virtual.name, line: span[:line], column: span[:column].to_i,
        end_line: span[:end_line].to_i, end_column: span[:end_column].to_i }
    end

    # The text and media type of a source by reference, or nil.
    def source(reference)
      reference = Integer(reference)
      virtual = @virtuals.each_value.find { |candidate| candidate&.reference == reference }
      return [virtual.text, SCXML] if virtual

      entry = @entries.each_value.find { |candidate| candidate.reference == reference }
      entry&.text && [entry.text, RUBY]
    end

    def modules = @entries.values.map(&:module_body)

    def sources = @entries.values.map(&:source_body)

    # The definition a DAP Source names, by reference or path, or nil.
    def definition_of(source)
      reference = source['sourceReference'].to_i
      entry = @entries.each_value.find do |candidate|
        reference.positive? ? candidate.reference == reference : (source['path'] && candidate.path == source['path'] && !candidate.retired)
      end
      entry&.definition
    end

    # The definitions that are not retired.
    def live = @entries.values.reject(&:retired).map(&:definition)

    # Notes the revisions `threads` run and the ones that were retired.
    # Answers [module events, loadedSource events] as [reason, body] pairs.
    def refresh(threads)
      modules = []
      sources = []
      threads.to_a.map(&:definition).uniq.each do |definition|
        next if @entries.key?(definition)

        entry = describe(definition)
        next if entry.nil?

        @entries[definition] = entry
        modules << ['new', entry.module_body]
        sources << ['new', entry.source_body]
      end
      @entries.each_value do |entry|
        next if entry.retired || !retired?(entry.definition)

        entry.retired = true
        modules << ['changed', entry.module_body]
        sources << ['changed', entry.source_body]
      end
      [modules, sources]
    end

    private

    def describe(definition)
      context = definition.context
      response = context.ta.definition.inspect(system: context.system_handle, definition: definition.id)
      return nil unless response[:result] == 'ok'

      facts = response.dig(:out, :out)
      family = facts.dig(:identity, :family, :name)
      revision = facts.dig(:identity, :revision).to_s
      virtual = virtual(definition)
      if virtual
        return Entry.new(definition: definition, family: family, revision: revision, chart: family, path: nil,
                         text: virtual.text, reference: virtual.reference, retired: false, virtual: virtual)
      end

      chart, = TinyactorsTest.charts.find { |_, candidate| (candidate.document_name || candidate.name).to_s == family }
      path = chart && @session.sources.file_of(chart)
      Entry.new(definition: definition, family: family, revision: revision,
                chart: chart || family, path: path, text: path && File.exist?(path) ? File.read(path) : nil,
                reference: (@next_reference += 1), retired: false)
    end

    # The encoded chart of a definition that keeps no spans, or nil when it
    # keeps some (its elements are in its files).
    def virtual(definition)
      return @virtuals[definition] if @virtuals.key?(definition)

      @virtuals[definition] = spans?(definition) ? nil : encode(definition)
    end

    def spans?(definition)
      Labels::KINDS.first(3).any? do |kind|
        (0...@labels.count(definition, kind)).any? { |id| @labels.span(definition, kind, id) }
      end
    end

    def encode(definition)
      context = definition.context
      response = context.ta.definition.encode(system: context.system_handle, definition: definition.id, codec: CODEC)
      return nil unless response[:result] == 'ok'

      family = @labels.identity(definition)&.dig(:family, :name) || 'chart'
      Virtual.new(reference: (@next_reference += 1), name: "#{family}.scxml", text: response.dig(:out, :out),
                  spans: response.dig(:out, :spans) || {})
    end

    # Retired, or freed because nothing uses it any more (ADR 0043).
    def retired?(definition)
      context = definition.context
      response = context.ta.definition.inspect(system: context.system_handle, definition: definition.id)
      response[:result] != 'ok' || response.dig(:out, :out, :retired)
    end
  end
end
