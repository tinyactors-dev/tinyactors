# frozen_string_literal: true

module DAP
  # Modules and loaded sources. A module is one registered definition
  # revision: its name is the definition family, its version the revision,
  # and a revision the workbench retired after a reload stays listed, marked
  # retired. A loaded source is the chart file a revision was compiled
  # from, with the text it had then, so a retired revision still shows the
  # chart it ran.
  class Catalog
    Entry = Struct.new(:definition, :family, :revision, :chart, :path, :text, :reference, :retired,
                       keyword_init: true) do
      def module_body
        body = { 'id' => definition.to_s, 'name' => retired ? "#{family} (retired)" : family, 'version' => revision }
        body['path'] = path if path
        body
      end

      def source_body
        body = { 'name' => "#{chart}.rb@#{revision.to_s[0, 8]}", 'sourceReference' => reference,
                 'presentationHint' => retired ? 'deemphasize' : 'normal' }
        body['name'] = "#{File.basename(path)}@#{revision.to_s[0, 8]}" if path
        body['path'] = path if path
        body
      end
    end

    def initialize(session)
      @session = session
      @entries = {}
      @next_reference = 0
    end

    def modules = @entries.values.map(&:module_body)

    def sources = @entries.values.map(&:source_body)

    def text(reference)
      entry = @entries.each_value.find { |candidate| candidate.reference == Integer(reference) }
      entry&.text
    end

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

    def context = @session.context

    def describe(definition)
      response = context.ta.definition.inspect(system: context.system_handle, definition: definition)
      return nil unless response[:result] == 'ok'

      facts = response.dig(:out, :out)
      family = facts.dig(:identity, :family, :name)
      chart, = TinyactorsTest.charts.find { |_, candidate| (candidate.document_name || candidate.name).to_s == family }
      path = chart && @session.sources.file_of(chart)
      Entry.new(definition: definition, family: family, revision: facts.dig(:identity, :revision).to_s,
                chart: chart || family, path: path, text: path && File.exist?(path) ? File.read(path) : nil,
                reference: (@next_reference += 1), retired: false)
    end

    # Retired, or freed because nothing uses it any more (ADR 0043).
    def retired?(definition)
      response = context.ta.definition.inspect(system: context.system_handle, definition: definition)
      response[:result] != 'ok' || response.dig(:out, :out, :retired)
    end
  end
end
