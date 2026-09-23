# frozen_string_literal: true

module TinyactorsTest
  # A statechart given as an SCXML document. The host imports it with
  # ta_definition_import() through the codec.scxml adapter (ADR 0053), so the
  # document is read by the SCXML codec that ships (tinyactors-scxml), not by
  # anything in Ruby.
  #
  # `datamodel:` picks the data-model adapter the host compiles with; the
  # document's own `datamodel` attribute, when it has one, must name the same.
  # `files:` are the resources a `<data src>` or `<script src>` may name: the
  # host's resolver.map serves them, since the core performs no I/O.
  class ScxmlChart
    attr_reader :name, :datamodel, :source, :files

    MEDIA_TYPE = 'application/scxml+xml'

    def initialize(name, source, datamodel: 'jq', files: {})
      @name = name.to_s
      @source = source.to_s
      @datamodel = datamodel&.to_s
      @files = files.transform_keys(&:to_s).transform_values(&:to_s)
    end

    # What the SCXML invoker is given for a `file:` src naming this chart.
    def invoker_entry = source

    def to_s = "#{name} (SCXML)"
  end
end
